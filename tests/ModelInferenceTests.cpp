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
//   4. SiFi fallback when SiFi-GAN is the selected engine but its model
//                    is missing while BigVGAN's is present, a render
//                    through EditorController must succeed via BigVGAN
//                    instead of failing (batch 6 review, M1)
//
// argv[1] is the models folder (Resources/models).

#include "../Source/Audio/BigVGANVocoder.h"
#include "../Source/Audio/EditorController.h"
#include "../Source/Audio/FCPEPitchDetector.h"
#include "../Source/Audio/SiFiGANVocoder.h"
#include "../Source/Audio/Synthesis/PsolaSynthesizer.h"
#include "../Source/Audio/SynthesisEngineType.h"
#include "../Source/Models/Project.h"
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

    // 4. SiFi-GAN selected but its model missing, BigVGAN present: the
    //    render must fall back to BigVGAN and succeed (batch 6 review
    //    M1 - before the fix, nothing loaded BigVGAN for a SiFi-GAN
    //    selection and the render failed with no AI route loaded).
    //    The controller resolves models through PlatformPaths, so the
    //    stage keys off the RESOLVED locations, not the argv folder.
    //    In the Windows CI both are Resources/models, where the BigVGAN
    //    conversion step has run and no SiFi-GAN files are placed.
    if (!PlatformPaths::getSiFiGanModelFile().existsAsFile() &&
        PlatformPaths::getBigVganModelFile().existsAsFile())
    {
        // Diagnostic first: can the BigVGAN vocoder itself load and
        // infer on this machine? If the fallback below fails, this
        // separates a vocoder problem from a render-path problem.
        {
            BigVGANVocoder probe;
            const bool probeLoaded = probe.loadModel(
                PlatformPaths::getBigVganModelFile(),
                PlatformPaths::getBigVganNoctModelFile());
            std::printf("BigVGAN probe: loaded %d, error '%s'\n",
                        probeLoaded ? 1 : 0, probe.getLastError().c_str());
            if (probeLoaded)
            {
                std::vector<std::vector<float>> dummyMel(
                    43, std::vector<float>(128, -5.0f));
                const auto out = probe.infer(dummyMel);
                std::printf("BigVGAN probe infer: %zu samples, error '%s'\n",
                            out.size(), probe.getLastError().c_str());
            }
        }

        EditorController controller(false);
        controller.setSynthesisEngineType(SynthesisEngineType::SiFiGAN);

        Project project;
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
        // The tone is voiced throughout; without a voiced mask the
        // PSOLA scratch render behind the BigVGAN route has no periods
        // to place and the render fails before BigVGAN is reached.
        audio.voicedMask.assign(static_cast<size_t>(frames), true);
        // The composed pitch curves the synthesizer reads the edited
        // F0 from (Project::getAdjustedF0ForRange), as analysis leaves
        // them for a note sung at 220 Hz: base at MIDI 57, no delta.
        // Without them the adjusted F0 range is empty and the render
        // request is refused before any engine runs.
        audio.basePitch.assign(static_cast<size_t>(frames), 57.0f);
        audio.deltaPitch.assign(static_cast<size_t>(frames), 0.0f);
        // The fallback route's gates ask for a non-empty analysis mel;
        // the BigVGAN render itself builds its own mel from the scratch
        // waveform, so the content is not read.
        audio.melSpectrogram.assign(static_cast<size_t>(frames),
                                    std::vector<float>(80, 0.0f));

        Note note(0, frames - 2, 57.0f); // A3, the tone's pitch
        note.setSrcStartFrame(0);
        note.setSrcEndFrame(frames - 2);
        note.setOriginalMidiNote(57.0f);
        note.setPitchOffset(static_cast<float>(kShiftSemitones));
        note.markDirty();
        project.addNote(note);

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
        // The BigVGAN render runs on a worker thread and reports back
        // through the message thread; on a CPU-only machine it can take
        // a while, so allow several minutes before giving up.
        for (int waited = 0; waited < 600 && !completed.load(); ++waited)
            juce::MessageManager::getInstance()->runDispatchLoopUntil(500);
        std::printf("SiFi fallback: completed %d, succeeded %d, synth "
                    "error '%s'\n",
                    completed.load() ? 1 : 0, succeeded.load() ? 1 : 0,
                    controller.getLastSynthesisError().c_str());
        CHECK(completed.load());
        CHECK(succeeded.load());
    }
    else
    {
        std::printf("SiFi fallback: needs SiFi-GAN missing and BigVGAN "
                    "present at the resolved model locations, stage "
                    "skipped\n");
    }

    std::printf("ModelInferenceTests: pitch detection and PSOLA work on this platform\n");
    return 0;
}
