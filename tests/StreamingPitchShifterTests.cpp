// Batch 2 (insert prototype): StreamingPitchShifter behaviour -
// bit-exact bypass at neutral, pitch ratio actually applied,
// formant-only processing is live, and the measured end-to-end delay
// matches the reported fixed latency.
#include "../Source/Audio/StreamingPitchShifter.h"
#include "TestAssert.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace
{
constexpr double kSampleRate = 44100.0;
constexpr double kPi = 3.14159265358979323846;

std::vector<float> renderBlocks(StreamingPitchShifter &shifter,
                                const std::vector<float> &input,
                                int blockSize = 512)
{
    std::vector<float> output(input.size(), 0.0f);
    juce::AudioBuffer<float> buffer(1, blockSize);
    size_t pos = 0;
    while (pos < input.size())
    {
        const int n = static_cast<int>(std::min<size_t>(blockSize, input.size() - pos));
        buffer.setSize(1, n, false, false, true);
        for (int i = 0; i < n; ++i)
            buffer.setSample(0, i, input[pos + i]);
        shifter.process(buffer);
        for (int i = 0; i < n; ++i)
            output[pos + i] = buffer.getSample(0, i);
        pos += n;
    }
    return output;
}

// Zero-crossing rate over [from, to) -> frequency in Hz.
double measureFrequency(const std::vector<float> &x, size_t from, size_t to)
{
    int crossings = 0;
    double firstCross = -1.0, lastCross = -1.0;
    for (size_t i = from + 1; i < to; ++i)
    {
        if ((x[i - 1] < 0.0f && x[i] >= 0.0f) || (x[i - 1] >= 0.0f && x[i] < 0.0f))
        {
            const double frac = std::abs(x[i - 1]) /
                                (std::abs(x[i - 1]) + std::abs(x[i]) + 1.0e-12);
            const double at = static_cast<double>(i - 1) + frac;
            if (firstCross < 0.0)
                firstCross = at;
            lastCross = at;
            ++crossings;
        }
    }
    if (crossings < 4 || lastCross <= firstCross)
        return 0.0;
    return (crossings - 1) * 0.5 * kSampleRate / (lastCross - firstCross);
}

double rms(const std::vector<float> &x, size_t from, size_t to)
{
    double sum = 0.0;
    for (size_t i = from; i < to; ++i)
        sum += static_cast<double>(x[i]) * x[i];
    return std::sqrt(sum / static_cast<double>(to - from));
}

std::vector<float> sine(double freq, size_t count, float amp = 0.7f)
{
    std::vector<float> x(count);
    for (size_t i = 0; i < count; ++i)
        x[i] = amp * static_cast<float>(std::sin(2.0 * kPi * freq * i / kSampleRate));
    return x;
}
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    // --- Bypass is bit-exact -------------------------------------------
    {
        StreamingPitchShifter shifter;
        shifter.prepare(kSampleRate, 1);
        shifter.setPitchPercent(100.0f);
        shifter.setFormantShiftSemitones(0.0f);

        std::vector<float> input(8192);
        juce::Random rng(42);
        for (auto &v : input)
            v = rng.nextFloat() * 2.0f - 1.0f;
        const auto output = renderBlocks(shifter, input);
        for (size_t i = 0; i < input.size(); ++i)
            CHECK(output[i] == input[i]);
    }

    // --- Pitch up: 220 Hz at 150% measures ~330 Hz ---------------------
    {
        StreamingPitchShifter shifter;
        shifter.prepare(kSampleRate, 1);
        shifter.setPitchPercent(150.0f);
        const auto input = sine(220.0, 44100);
        const auto output = renderBlocks(shifter, input);

        const double measured = measureFrequency(output, 16000, 40000);
        std::printf("pitch 150%%: measured %.1f Hz (expect ~330)\n", measured);
        CHECK(measured > 330.0 * 0.96 && measured < 330.0 * 1.04);
        CHECK(rms(output, 16000, 40000) > 0.2);
        for (float v : output)
            CHECK(std::isfinite(v));
    }

    // --- Pitch down: 220 Hz at 50% measures ~110 Hz --------------------
    {
        StreamingPitchShifter shifter;
        shifter.prepare(kSampleRate, 1);
        shifter.setPitchPercent(50.0f);
        const auto input = sine(220.0, 44100);
        const auto output = renderBlocks(shifter, input);
        const double measured = measureFrequency(output, 16000, 40000);
        std::printf("pitch 50%%: measured %.1f Hz (expect ~110)\n", measured);
        CHECK(measured > 110.0 * 0.95 && measured < 110.0 * 1.05);
    }

    // --- Formant-only processing is live (not a silent bypass) ---------
    {
        StreamingPitchShifter shifter;
        shifter.prepare(kSampleRate, 1);
        shifter.setPitchPercent(100.0f);
        shifter.setFormantShiftSemitones(6.0f);
        const auto input = sine(220.0, 22050);
        const auto output = renderBlocks(shifter, input);
        double maxDiff = 0.0;
        for (size_t i = 4096; i < input.size(); ++i)
            maxDiff = std::max(maxDiff, static_cast<double>(std::abs(output[i] - input[i])));
        CHECK(maxDiff > 0.05); // processed, not passed through
        CHECK(rms(output, 8192, 20000) > 0.15);
    }

    // --- Measured latency matches the reported fixed latency ------------
    {
        StreamingPitchShifter shifter;
        shifter.prepare(kSampleRate, 1);
        shifter.setPitchPercent(150.0f);
        constexpr size_t kImpulseAt = 8192;
        std::vector<float> input(32768, 0.0f);
        input[kImpulseAt] = 1.0f;
        const auto output = renderBlocks(shifter, input);

        // An isolated impulse is smeared over a few grains, so measure
        // the energy centroid of the response rather than a peak.
        double weightSum = 0.0, moment = 0.0;
        for (size_t i = kImpulseAt; i < output.size(); ++i)
        {
            const double w = std::abs(output[i]);
            weightSum += w;
            moment += w * static_cast<double>(i);
        }
        const int reported = shifter.getLatencySamples();
        const double centroid = weightSum > 0.0 ? moment / weightSum : 0.0;
        const double measuredDelay = centroid - static_cast<double>(kImpulseAt);
        std::printf("latency: reported %d samples (%.2f ms), impulse centroid "
                    "delay %.0f samples, response weight %.2f\n",
                    reported, reported * 1000.0 / kSampleRate, measuredDelay,
                    weightSum);
        CHECK(weightSum > 0.2);
        CHECK(std::abs(measuredDelay - reported) <= 300.0);
    }

    std::printf("StreamingPitchShifter tests passed\n");
    return 0;
}
