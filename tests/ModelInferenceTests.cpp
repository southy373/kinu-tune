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
//
// argv[1] is the models folder (Resources/models).

#include "../Source/Audio/FCPEPitchDetector.h"
#include "../Source/Audio/Synthesis/PsolaSynthesizer.h"
#include "../Source/Audio/SynthesisEngineType.h"
#include "../Source/Utils/Constants.h"
#include "../Source/Utils/OnnxRuntime.h"
#include "TestAssert.h"

#include <algorithm>
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

    std::printf("ModelInferenceTests: pitch detection and PSOLA work on this platform\n");
    return 0;
}
