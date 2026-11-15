// Kinu Tune - Copyright (C) 2026 Keinoaza
// Part of Kinu Tune, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.

// Loads the model Kinu Tune ships (FCPE) and checks, with numbers, that pitch
// detection and the Classic (PSOLA) engine work on the platform this runs on.
// The GAME and PC-NSF-HiFiGAN models (CC BY-NC-SA 4.0) are not part of the
// speech build, so they are no longer tested here.
//
// A two-second harmonic tone at 220 Hz (A3, MIDI 57) goes through:
//   1. FCPE          the pitch detector must report 220 Hz
//   2. PSOLA         the Classic engine shifting the tone by a minor third must
//                    sound at 261.6 Hz (measured with FCPE)
//   3. SiFi-GAN      when its model is in the models folder, the Plan B
//                    engine generating from an edited F0 must sound at that
//                    F0 (220 Hz as-is, 440 Hz an octave up)
//   4. SiFi no-fallback
//                    when SiFi-GAN is the selected engine but its model
//                    is missing while BigVGAN's is present AND loaded, a
//                    render through EditorController must FAIL with a
//                    SiFi-GAN error and must not call BigVGAN (batch 6.1:
//                    no silent fallback - in 0.6.0 this render silently
//                    succeeded through BigVGAN)
//   5. SiFi dispatch with BOTH vocoders loaded and SiFi-GAN selected,
//                    the render must run through SiFi-GAN and must not
//                    call BigVGAN (batch 6.1: the exact configuration
//                    0.6.0 got wrong on real installs)
//   6. SiFi gate     the synthesizer itself, given a loaded BigVGAN and
//                    an unloaded SiFi-GAN with SiFi-GAN selected, must
//                    fail with a SiFi-GAN error and not call BigVGAN
//
// argv[1] is the models folder (Resources/models).

#include "../Source/Audio/BigVGANVocoder.h"
#include "../Source/Audio/EditorController.h"
#include "../Source/Audio/FCPEPitchDetector.h"
#include "../Source/Audio/SiFiGANVocoder.h"
#include "../Source/Audio/Synthesis/IncrementalSynthesizer.h"
#include "../Source/Audio/Synthesis/PsolaSynthesizer.h"
#include "../Source/Audio/SynthesisEngineType.h"
#include "../Source/Models/Project.h"
#include "../Source/Utils/AppLogger.h"
#include "../Source/Utils/Constants.h"
#include "../Source/Utils/OnnxRuntime.h"
#include "../Source/Utils/PlatformPaths.h"
#include "TestAssert.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <vector>

namespace
{
constexpr int kSampleRate = 44100;
constexpr double kSourceHz = 220.0; // A3
constexpr double kSeconds = 2.0;
constexpr double kShiftSemitones = 3.0; // a minor third, as the harmony buttons use
constexpr double kTolerancePercent = 3.0; // about half a semitone

double shifted(double hz) { return hz * std::pow(2.0, kShiftSemitones / 12.0); }

// A tone with harmonics, a slow vibrato, a little tremolo and a soft start and
// end: near enough to a sung vowel for the models, which were trained on
// singing (a bare steady sine is "not a voice" to the note segmenter).
std::vector<float> makeTone(double hz)
{
    const auto numSamples = static_cast<size_t>(kSeconds * kSampleRate);
    std::vector<float> tone(numSamples);
    const double fade = 0.05 * kSampleRate;
    const double pi = juce::MathConstants<double>::pi;
    constexpr double vibratoRateHz = 5.5;
    constexpr double vibratoDepthCents = 40.0;
    constexpr double tremoloDepth = 0.1;
    double phase = 0.0; // of the fundamental, advanced sample by sample so the vibrato is a true frequency modulation
    for (size_t i = 0; i < numSamples; ++i)
    {
        const double t = static_cast<double>(i) / kSampleRate;
        const double cents = vibratoDepthCents * std::sin(2.0 * pi * vibratoRateHz * t);
        const double instantHz = hz * std::pow(2.0, cents / 1200.0);
        phase += 2.0 * pi * instantHz / kSampleRate;
        double value = 0.0;
        for (int harmonic = 1; harmonic <= 8; ++harmonic)
            value += std::sin(phase * harmonic) / harmonic;
        value *= 1.0 - tremoloDepth * 0.5 * (1.0 - std::cos(2.0 * pi * vibratoRateHz * t));
        double envelope = 1.0;
        if (static_cast<double>(i) < fade)
            envelope = static_cast<double>(i) / fade;
        else if (static_cast<double>(numSamples - 1 - i) < fade)
            envelope = static_cast<double>(numSamples - 1 - i) / fade;
        tone[i] = static_cast<float>(0.25 * envelope * value);
    }
    return tone;
}

// The median of the voiced frames: one number for "what pitch is this".
float voicedMedianHz(const std::vector<float> &f0)
{
    std::vector<float> voiced;
    for (float hz : f0)
        if (hz > 0.0f)
            voiced.push_back(hz);
    if (voiced.empty())
        return 0.0f;
    std::sort(voiced.begin(), voiced.end());
    return voiced[voiced.size() / 2];
}

size_t voicedCount(const std::vector<float> &f0)
{
    return static_cast<size_t>(std::count_if(f0.begin(), f0.end(), [](float hz) { return hz > 0.0f; }));
}

bool withinPercent(double value, double target, double percent)
{
    return std::abs(value - target) <= target * percent / 100.0;
}

float peak(const std::vector<float> &audio)
{
    float result = 0.0f;
    for (float sample : audio)
        result = std::max(result, std::abs(sample));
    return result;
}

// Pitch of a rendered signal, measured the same way the plug-in analyses audio.
float measureHz(FCPEPitchDetector &fcpe, const std::vector<float> &audio, int sampleRate)
{
    const auto f0 = fcpe.extractF0(audio.data(), static_cast<int>(audio.size()), sampleRate);
    return voicedMedianHz(f0);
}

// How many inference runs a vocoder's always-on session log records. Each
// vocoder logs one "infer [N frames]" line per inference and flushes it,
// so counting the lines added across a render tells which engine actually
// ran - the same evidence the 0.6.0 field diagnosis was made from.
int countInferLines(const juce::String &logFileName, const juce::String &marker)
{
    const auto file = PlatformPaths::getLogFile(logFileName);
    if (!file.existsAsFile())
        return 0;
    const auto text = file.loadFileAsString();
    int count = 0;
    int from = 0;
    while (true)
    {
        const int at = text.indexOf(from, marker);
        if (at < 0)
            break;
        ++count;
        from = at + marker.length();
    }
    return count;
}

int bigVganInferCount()
{
    return countInferLines("bigvgan_" + AppLogger::getSessionId() + ".txt",
                           "BigVGAN infer [");
}

int siFiInferCount()
{
    return countInferLines("sifigan_" + AppLogger::getSessionId() + ".txt",
                           "SiFi-GAN infer [");
}

// The tone as an analysed project with one dirty edited note, as the
// controller render stages below need it.
void fillToneProject(Project &project, const std::vector<float> &tone)
{
    auto &audio = project.getAudioData();
    audio.sampleRate = kSampleRate;
    audio.waveform.setSize(1, static_cast<int>(tone.size()));
    audio.originalWaveform.setSize(1, static_cast<int>(tone.size()));
    for (size_t i = 0; i < tone.size(); ++i)
    {
        audio.waveform.setSample(0, static_cast<int>(i), tone[i]);
        audio.originalWaveform.setSample(0, static_cast<int>(i), tone[i]);
    }
    const int frames = static_cast<int>(tone.size()) / HOP_SIZE;
    audio.f0.assign(static_cast<size_t>(frames),
                    static_cast<float>(kSourceHz));
    audio.denseF0 = audio.f0;
    // The tone is voiced throughout; without a voiced mask the PSOLA
    // scratch render behind the BigVGAN route has no periods to place
    // and the render fails before BigVGAN is reached.
    audio.voicedMask.assign(static_cast<size_t>(frames), true);
    // The composed pitch curves the synthesizer reads the edited F0 from
    // (Project::getAdjustedF0ForRange), as analysis leaves them for a
    // note sung at 220 Hz: base at MIDI 57, no delta. Without them the
    // adjusted F0 range is empty and the render request is refused
    // before any engine runs.
    audio.basePitch.assign(static_cast<size_t>(frames), 57.0f);
    audio.deltaPitch.assign(static_cast<size_t>(frames), 0.0f);
    // The AI routes' gates ask for a non-empty analysis mel; the renders
    // themselves build what they need, so the content is not read.
    audio.melSpectrogram.assign(static_cast<size_t>(frames),
                                std::vector<float>(80, 0.0f));

    Note note(0, frames - 2, 57.0f); // A3, the tone's pitch
    note.setSrcStartFrame(0);
    note.setSrcEndFrame(frames - 2);
    note.setOriginalMidiNote(57.0f);
    note.setPitchOffset(static_cast<float>(kShiftSemitones));
    note.markDirty();
    project.addNote(note);
}

// One render through the controller, waiting out the worker thread the
// render runs on. Returns whether the render reported success.
bool runControllerRender(EditorController &controller, Project &project)
{
    std::atomic<bool> pendingRerun{false};
    std::atomic<bool> completed{false};
    std::atomic<bool> succeeded{false};
    controller.resynthesizeIncrementalAsync(
        project, nullptr,
        [&](bool ok)
        {
            succeeded.store(ok);
            completed.store(true);
        },
        pendingRerun, false);
    // The render runs on a worker thread and reports back through the
    // message thread; on a CPU-only machine it can take a while, so
    // allow several minutes before giving up.
    for (int waited = 0; waited < 600 && !completed.load(); ++waited)
        juce::MessageManager::getInstance()->runDispatchLoopUntil(500);
    CHECK(completed.load());
    return succeeded.load();
}
} // namespace

int main(int argc, char **argv)
{
    juce::ScopedJuceInitialiser_GUI gui;

    CHECK(argc >= 2); // the models folder
    const juce::File models(juce::String::fromUTF8(argv[1]));
    CHECK(models.isDirectory());

    CHECK(OnnxRuntime::useLinkedRuntimeForTesting());
    CHECK(OnnxRuntime::isAvailable());

    const auto tone = makeTone(kSourceHz);

    // 1. FCPE finds the tone's pitch.
    FCPEPitchDetector fcpe;
    CHECK(fcpe.loadModel(models.getChildFile("fcpe.onnx"),
                         models.getChildFile("mel_filterbank.bin"),
                         models.getChildFile("cent_table.bin")));
    const auto f0 = fcpe.extractF0(tone.data(), static_cast<int>(tone.size()), kSampleRate);
    const float detectedHz = voicedMedianHz(f0);
    std::printf("FCPE: %zu frames, %zu voiced, median %.1f Hz (expected %.1f)\n",
                f0.size(), voicedCount(f0), detectedHz, kSourceHz);
    CHECK(!f0.empty());
    CHECK(voicedCount(f0) * 2 > f0.size());
    CHECK(withinPercent(detectedHz, kSourceHz, kTolerancePercent));

    // 2. PSOLA (the Classic engine) shifts the tone by a minor third.
    const double targetHz = shifted(kSourceHz);
    DspSynthesisRequest request;
    request.source = tone;
    request.sourceStartSample = 0;
    request.hopSize = HOP_SIZE;
    request.sampleRate = kSampleRate;
    request.numOutputFrames = static_cast<int>(tone.size()) / HOP_SIZE;
    const int sourceFrames = request.numOutputFrames + 2;
    request.sourceF0.assign(static_cast<size_t>(sourceFrames), static_cast<float>(kSourceHz));
    request.sourceVoiced.assign(static_cast<size_t>(sourceFrames), 1);
    request.sourceFrame.resize(static_cast<size_t>(request.numOutputFrames));
    for (int frame = 0; frame < request.numOutputFrames; ++frame)
        request.sourceFrame[static_cast<size_t>(frame)] = static_cast<double>(frame);
    request.pitchRatio.assign(static_cast<size_t>(request.numOutputFrames),
                              static_cast<float>(targetHz / kSourceHz));
    const auto psola = PsolaSynthesizer::render(request);
    const float psolaHz = measureHz(fcpe, psola, kSampleRate);
    std::printf("PSOLA: %zu samples (expected %d), peak %.3f, %.1f Hz (expected %.1f)\n",
                psola.size(), request.numOutputFrames * HOP_SIZE, peak(psola), psolaHz, targetHz);
    CHECK(psola.size() == static_cast<size_t>(request.numOutputFrames * HOP_SIZE));
    CHECK(peak(psola) > 0.01f);
    CHECK(withinPercent(psolaHz, targetHz, kTolerancePercent));

    // 3. SiFi-GAN (the Plan B engine), when its model and stats sidecar
    //    are in the models folder: the tone generated from its own pitch
    //    measures at 220 Hz, and the same tone generated from an edited
    //    F0 an octave up measures at 440 Hz - the pitch arrives as data,
    //    so no pitch-shifted input is involved in either render.
    const auto siFiModel = models.getChildFile("sifigan_115k_gather.onnx");
    const auto siFiStats = models.getChildFile("sifigan_kokoro_stats.json");
    if (siFiModel.existsAsFile() && siFiStats.existsAsFile())
    {
        SiFiGANVocoder siFi;
        CHECK(siFi.loadModel(siFiModel, siFiStats));

        SiFiGANVocoder::RenderRequest request;
        request.sourceWaveform = tone;
        request.sourceSampleRate = kSampleRate;
        request.analysisHop = HOP_SIZE;
        request.numOutputFrames = static_cast<int>(tone.size()) / HOP_SIZE;
        const int trackFrames = request.numOutputFrames + 2;
        request.sourceF0.assign(static_cast<size_t>(trackFrames),
                                static_cast<float>(kSourceHz));
        request.targetF0.assign(static_cast<size_t>(trackFrames),
                                static_cast<float>(kSourceHz));

        // The render length is the output frame count converted onto
        // the 24k/120 grid, which is fractional in analysis frames, so
        // the result can fall short of the exact sample count by up to
        // one SiFi frame (120 samples at 24k, 221 at 44.1k) plus
        // resampler edge rounding. The synthesizer's contract allows a
        // short render - it pads the difference from the original - so
        // the check allows the same.
        const auto expectedSamples =
            static_cast<size_t>(request.numOutputFrames * HOP_SIZE);
        const auto lengthOk = [expectedSamples](size_t rendered)
        {
            return rendered <= expectedSamples &&
                   expectedSamples - rendered <= 256;
        };

        const auto samePitch = siFi.inferSync(request);
        const float sameHz = measureHz(fcpe, samePitch, kSampleRate);
        std::printf("SiFi-GAN 1.0x: %zu samples (expected %zu), peak %.3f, %.1f Hz (expected %.1f)\n",
                    samePitch.size(), expectedSamples,
                    peak(samePitch), sameHz, kSourceHz);
        CHECK(lengthOk(samePitch.size()));
        CHECK(peak(samePitch) > 0.01f);
        CHECK(withinPercent(sameHz, kSourceHz, kTolerancePercent));

        request.targetF0.assign(static_cast<size_t>(trackFrames),
                                static_cast<float>(2.0 * kSourceHz));
        const auto octaveUp = siFi.inferSync(request);
        const float octaveHz = measureHz(fcpe, octaveUp, kSampleRate);
        std::printf("SiFi-GAN 2.0x: %zu samples, peak %.3f, %.1f Hz (expected %.1f)\n",
                    octaveUp.size(), peak(octaveUp), octaveHz, 2.0 * kSourceHz);
        CHECK(lengthOk(octaveUp.size()));
        CHECK(peak(octaveUp) > 0.01f);
        CHECK(withinPercent(octaveHz, 2.0 * kSourceHz, kTolerancePercent));
    }
    else
    {
        std::printf("SiFi-GAN: model not in the models folder, stage skipped\n");
    }

    // 4. SiFi-GAN selected but its model missing, BigVGAN present AND
    //    loaded: the render must FAIL with a SiFi-GAN error, and BigVGAN
    //    must not be called (batch 6.1, no silent fallback - 0.6.0
    //    silently rendered this through BigVGAN and reported success).
    //    The controller resolves models through PlatformPaths, so the
    //    stage keys off the RESOLVED locations, not the argv folder.
    if (!PlatformPaths::getSiFiGanModelFile().existsAsFile() &&
        PlatformPaths::getBigVganModelFile().existsAsFile())
    {
        EditorController controller(false);

        // The real-install state: BigVGAN's model is loaded, as any
        // earlier AI Resynthesis use leaves it. Loading it explicitly
        // here is what makes this stage the configuration 0.6.0 got
        // wrong, rather than the nothing-loaded case.
        auto *bigVgan = controller.getBigVganVocoder();
        CHECK(bigVgan != nullptr);
        CHECK(bigVgan->loadModel(PlatformPaths::getBigVganModelFile(),
                                 PlatformPaths::getBigVganNoctModelFile()));

        controller.setSynthesisEngineType(SynthesisEngineType::SiFiGAN);

        Project project;
        fillToneProject(project, tone);

        const int bigVganBefore = bigVganInferCount();
        const bool succeeded = runControllerRender(controller, project);
        const auto error = controller.getLastSynthesisError();
        std::printf("SiFi no-fallback: succeeded %d, synth error '%s', "
                    "BigVGAN infers during render %d\n",
                    succeeded ? 1 : 0, error.c_str(),
                    bigVganInferCount() - bigVganBefore);
        CHECK(!succeeded);
        CHECK(error.find("SiFi-GAN") != std::string::npos);
        CHECK(bigVganInferCount() == bigVganBefore);
    }
    else
    {
        std::printf("SiFi no-fallback: needs SiFi-GAN missing and BigVGAN "
                    "present at the resolved model locations, stage "
                    "skipped\n");
    }

    // 5. BOTH vocoders loaded, SiFi-GAN selected: the render must run
    //    through SiFi-GAN and must not call BigVGAN. This is the exact
    //    configuration of a real install after AI Resynthesis has been
    //    used once - the one 0.6.0 silently routed to BigVGAN.
    if (PlatformPaths::getSiFiGanModelFile().existsAsFile() &&
        PlatformPaths::getBigVganModelFile().existsAsFile())
    {
        EditorController controller(false);

        Project project;
        fillToneProject(project, tone);

        // Warm-up: an AI Resynthesis (Vocoder engine) render, which is
        // what loads the BigVGAN model on a real machine before the
        // user ever selects SiFi-GAN.
        controller.setSynthesisEngineType(SynthesisEngineType::Vocoder);
        CHECK(runControllerRender(controller, project));
        CHECK(controller.getBigVganVocoder() != nullptr &&
              controller.getBigVganVocoder()->isLoaded());

        controller.setSynthesisEngineType(SynthesisEngineType::SiFiGAN);
        // The warm-up render consumed the dirty marks; the note's edit
        // still reaches the audio, so this re-marks it for the SiFi-GAN
        // render.
        CHECK(project.markEditedNotesDirty() > 0);

        const int bigVganBefore = bigVganInferCount();
        const int siFiBefore = siFiInferCount();
        const bool succeeded = runControllerRender(controller, project);
        std::printf("SiFi dispatch: succeeded %d, synth error '%s', "
                    "SiFi-GAN infers %d, BigVGAN infers %d\n",
                    succeeded ? 1 : 0,
                    controller.getLastSynthesisError().c_str(),
                    siFiInferCount() - siFiBefore,
                    bigVganInferCount() - bigVganBefore);
        CHECK(succeeded);
        CHECK(controller.getSiFiGanVocoder() != nullptr &&
              controller.getSiFiGanVocoder()->isLoaded());
        CHECK(siFiInferCount() > siFiBefore);
        CHECK(bigVganInferCount() == bigVganBefore);
    }
    else
    {
        std::printf("SiFi dispatch: needs SiFi-GAN and BigVGAN present "
                    "at the resolved model locations, stage skipped\n");
    }

    // 6. The synthesizer's own gate, without the controller in front of
    //    it: loaded BigVGAN, unloaded SiFi-GAN, SiFi-GAN selected. The
    //    render must fail with a SiFi-GAN error and BigVGAN must not be
    //    called - render paths that reach the synthesizer without the
    //    controller's load attempt are covered by this gate.
    if (PlatformPaths::getBigVganModelFile().existsAsFile())
    {
        BigVGANVocoder bigVgan;
        CHECK(bigVgan.loadModel(PlatformPaths::getBigVganModelFile(),
                                PlatformPaths::getBigVganNoctModelFile()));
        SiFiGANVocoder siFi; // never loaded

        Project project;
        fillToneProject(project, tone);

        IncrementalSynthesizer synth;
        synth.setProject(&project);
        synth.setBigVganVocoder(&bigVgan);
        synth.setSiFiGanVocoder(&siFi);
        synth.setSynthesisEngine(SynthesisEngineType::SiFiGAN);

        const int bigVganBefore = bigVganInferCount();
        std::atomic<bool> completed{false};
        std::atomic<bool> succeeded{false};
        synth.synthesizeRegion(
            nullptr,
            [&](bool ok)
            {
                succeeded.store(ok);
                completed.store(true);
            });
        for (int waited = 0; waited < 600 && !completed.load(); ++waited)
            juce::MessageManager::getInstance()->runDispatchLoopUntil(500);
        const auto error = synth.getLastSynthesisError();
        std::printf("SiFi synth gate: completed %d, succeeded %d, error "
                    "'%s', BigVGAN infers during render %d\n",
                    completed.load() ? 1 : 0, succeeded.load() ? 1 : 0,
                    error.c_str(), bigVganInferCount() - bigVganBefore);
        CHECK(completed.load());
        CHECK(!succeeded.load());
        CHECK(error.find("SiFi-GAN") != std::string::npos);
        CHECK(bigVganInferCount() == bigVganBefore);
    }
    else
    {
        std::printf("SiFi synth gate: needs BigVGAN present at the "
                    "resolved model locations, stage skipped\n");
    }

    std::printf("ModelInferenceTests: pitch detection and PSOLA work on this platform\n");
    return 0;
}
