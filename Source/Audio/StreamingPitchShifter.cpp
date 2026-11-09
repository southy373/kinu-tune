#include "StreamingPitchShifter.h"

#include <algorithm>
#include <cmath>

namespace
{
constexpr double kPi = 3.14159265358979323846;
}

void StreamingPitchShifter::prepare(double newSampleRate, int channels)
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;
    numChannels = juce::jlimit(1, 2, channels);
    inputRings.assign(static_cast<size_t>(numChannels),
                      std::vector<float>(static_cast<size_t>(kRingSize), 0.0f));
    interRings.assign(static_cast<size_t>(numChannels),
                      std::vector<float>(static_cast<size_t>(kRingSize), 0.0f));
    monoRing.assign(static_cast<size_t>(kRingSize), 0.0f);
    analysisScratch.assign(static_cast<size_t>(kAnalysisWindow), 0.0f);
    nsdfScratch.assign(static_cast<size_t>(kAnalysisWindow / 2 + 2), 0.0f);
    reset();
}

void StreamingPitchShifter::reset()
{
    for (auto &ring : inputRings)
        std::fill(ring.begin(), ring.end(), 0.0f);
    for (auto &ring : interRings)
        std::fill(ring.begin(), ring.end(), 0.0f);
    std::fill(monoRing.begin(), monoRing.end(), 0.0f);

    inputWritten = 0;
    nextAnalysisEnd = kAnalysisWindow;
    currentPeriod = 160.0;
    voiced = false;

    // No ratio-dependent seeding here. The pitch percent can still
    // change between prepare()/reset() and the first process() call -
    // the host restores parameters in between - and seeding the grain
    // chain with that stale ratio while the reader seeds with the live
    // one put the two scales 128 * (stale / live - 1) input samples
    // apart, shifting the effective delay away from the reported
    // kLatencySamples. process() seeds both from the ratio in effect
    // when audio actually starts.
    nextInputMark = 0.0;
    nextInterPos = 0.0;
    interValidFrom = 0;
    interValidTo = 0;
    readerPos = 0.0;
    streamSeeded = false;
    outputWritten = 0;
}

void StreamingPitchShifter::process(juce::AudioBuffer<float> &buffer)
{
    const int numSamples = buffer.getNumSamples();
    if (numChannels <= 0 || numSamples <= 0)
        return;

    const double ratio = pitchPercent / 100.0;
    const bool bypass = std::abs(ratio - 1.0) < 1.0e-6 &&
                        std::abs(formantSemitones) < 1.0e-6f;

    if (!streamSeeded)
    {
        // Seed the grain chain and the reader with the one ratio in
        // effect when audio starts: the first grain is centred early
        // in the stream (nominal-period grains cover the input until
        // the first analysis window lands), the intermediate stream
        // uses the absolute scale (position p corresponds to input
        // p / ratio), and output sample n reads the intermediate at
        // (n - latency) * ratio - so the end-to-end delay is exactly
        // kLatencySamples regardless of any parameter changes between
        // prepare() and now.
        nextInputMark = 128.0;
        nextInterPos = nextInputMark * ratio;
        interValidFrom = static_cast<juce::int64>(nextInterPos);
        interValidTo = interValidFrom;
        readerPos = -static_cast<double>(kLatencySamples) * ratio;
        streamSeeded = true;
    }

    pushInput(buffer);
    runAnalysis();
    runSynthesis();

    if (bypass)
    {
        // Dry signal leaves untouched (bit-exact); the pipeline state
        // above still advanced so a later shift engages in sync.
        readerPos += static_cast<double>(numSamples) * ratio;
        outputWritten += numSamples;
        return;
    }

    const int channelsToWrite = juce::jmin(numChannels, buffer.getNumChannels());
    for (int n = 0; n < numSamples; ++n)
    {
        const bool inRange = readerPos >= static_cast<double>(interValidFrom) &&
                             readerPos < static_cast<double>(interValidTo) - 1.0;
        for (int ch = 0; ch < channelsToWrite; ++ch)
        {
            float *out = buffer.getWritePointer(ch);
            out[n] = inRange ? readRing(interRings[static_cast<size_t>(ch)], readerPos)
                             : 0.0f;
        }
        readerPos += ratio;
    }
    outputWritten += numSamples;
}

void StreamingPitchShifter::pushInput(const juce::AudioBuffer<float> &buffer)
{
    const int numSamples = buffer.getNumSamples();
    const int channelsToRead = juce::jmin(numChannels, buffer.getNumChannels());
    for (int n = 0; n < numSamples; ++n)
    {
        const auto ringIndex = static_cast<size_t>((inputWritten + n) & kRingMask);
        float mono = 0.0f;
        for (int ch = 0; ch < channelsToRead; ++ch)
        {
            const float v = buffer.getReadPointer(ch)[n];
            inputRings[static_cast<size_t>(ch)][ringIndex] = v;
            mono += v;
        }
        if (channelsToRead > 1)
            mono *= 0.5f;
        monoRing[ringIndex] = mono;
    }
    inputWritten += numSamples;
}

void StreamingPitchShifter::runAnalysis()
{
    while (nextAnalysisEnd <= inputWritten)
    {
        // Copy the window ending at nextAnalysisEnd out of the ring.
        const juce::int64 start = nextAnalysisEnd - kAnalysisWindow;
        for (int i = 0; i < kAnalysisWindow; ++i)
            analysisScratch[static_cast<size_t>(i)] =
                monoRing[static_cast<size_t>((start + i) & kRingMask)];

        const float estimate = estimatePeriodSamples();
        if (estimate > 0.0f)
        {
            // Light smoothing keeps grain spacing from jittering.
            currentPeriod = voiced ? 0.5 * currentPeriod + 0.5 * estimate
                                   : static_cast<double>(estimate);
            voiced = true;
        }
        else
        {
            voiced = false;
        }
        nextAnalysisEnd += kAnalysisHop;
    }
}

float StreamingPitchShifter::estimatePeriodSamples()
{
    const float *x = analysisScratch.data();
    const int minLag = juce::jmax(16, static_cast<int>(sampleRate / 420.0));
    const int maxLag = juce::jmin(kAnalysisWindow / 2,
                                  static_cast<int>(sampleRate / 55.0));

    // NSDF (normalised autocorrelation) over the lag range, computed
    // once into the scratch array; the per-lag energies are accumulated
    // alongside the correlation, so nothing is allocated here.
    float bestNsdf = 0.0f;
    int bestLag = -1;
    for (int lag = minLag; lag <= maxLag; ++lag)
    {
        double corr = 0.0, eLeft = 0.0, eRight = 0.0;
        const int count = kAnalysisWindow - lag;
        for (int i = 0; i < count; ++i)
        {
            const double a = x[i], b = x[i + lag];
            corr += a * b;
            eLeft += a * a;
            eRight += b * b;
        }
        const double denom = eLeft + eRight;
        const float nsdf = denom > 1.0e-12 ? static_cast<float>(2.0 * corr / denom)
                                           : 0.0f;
        nsdfScratch[static_cast<size_t>(lag)] = nsdf;
        if (nsdf > bestNsdf)
        {
            bestNsdf = nsdf;
            bestLag = lag;
        }
    }
    if (bestLag < 0 || bestNsdf < 0.5f)
        return -1.0f; // unvoiced / silence

    // Anti period-doubling, MPM-style: take the smallest LOCAL maximum
    // that reaches 90% of the global peak. (Taking the first lag on the
    // rising slope instead biases the period short - on a sine it read
    // ~186 for a true 200.5, which breaks grain phase coherence.)
    int chosen = bestLag;
    for (int lag = juce::jmax(1, minLag); lag <= bestLag; ++lag)
    {
        const float v = nsdfScratch[static_cast<size_t>(lag)];
        const bool isLocalMax =
            v >= nsdfScratch[static_cast<size_t>(lag - 1)] &&
            (lag == maxLag || v >= nsdfScratch[static_cast<size_t>(lag + 1)]);
        if (isLocalMax && v >= 0.9f * bestNsdf)
        {
            chosen = lag;
            break;
        }
    }

    // Parabolic refinement around the chosen lag.
    const float ym = nsdfScratch[static_cast<size_t>(chosen - 1)];
    const float y0 = nsdfScratch[static_cast<size_t>(chosen)];
    const float yp = chosen < maxLag ? nsdfScratch[static_cast<size_t>(chosen + 1)]
                                     : y0;
    const float denomP = ym - 2.0f * y0 + yp;
    float shift = 0.0f;
    if (std::abs(denomP) > 1.0e-9f)
        shift = juce::jlimit(-0.5f, 0.5f, 0.5f * (ym - yp) / denomP);
    return static_cast<float>(chosen) + shift;
}

void StreamingPitchShifter::runSynthesis()
{
    const double ratio = pitchPercent / 100.0;
    const double formantRatio = std::pow(2.0, formantSemitones / 12.0);

    for (;;)
    {
        // Unvoiced stretches use a fixed nominal period so transients
        // and noise pass through the same machinery.
        const double period = voiced ? currentPeriod : 128.0;
        const double grainLength = juce::jlimit(
            32.0, 2048.0, 2.0 * period / formantRatio);
        const double half = grainLength * 0.5;

        // The grain's input extent must already have arrived (with
        // headroom for the alignment search below).
        if (nextInputMark + half + period * 0.5 > static_cast<double>(inputWritten))
            break;

        // Similarity alignment (the WSOLA ingredient): textbook PSOLA
        // spacing by period * ratio leaves consecutive grains
        // phase-incoherent in their overlap, which tears the waveform
        // apart at ratios like 1.5 (verified numerically against a
        // reference implementation). So the analysis centre is nudged
        // by up to half a period to maximise the correlation between
        // the grain's head and the intermediate tail it will overlap.
        // The synthesis position and the ideal (long-term) consumption
        // position never move, so the stretch ratio cannot drift.
        double analysisMark = nextInputMark;
        const double spacing = period * ratio;
        const int overlap = static_cast<int>(std::floor(grainLength - spacing));
        if (overlap > 8 && interValidTo > interValidFrom)
        {
            const juce::int64 outStart =
                static_cast<juce::int64>(std::llround(nextInterPos - half));
            const auto &in0 = inputRings[0];
            const auto &out0 = interRings[0];
            const int searchRadius = static_cast<int>(period * 0.5);
            double bestCorr = -1.0e30;
            int bestOffset = 0;
            for (int offset = -searchRadius; offset <= searchRadius; ++offset)
            {
                const juce::int64 inStart = static_cast<juce::int64>(
                    std::llround(nextInputMark - half)) + offset;
                double corr = 0.0;
                for (int i = 0; i < overlap; ++i)
                {
                    corr += static_cast<double>(
                                out0[static_cast<size_t>((outStart + i) & kRingMask)]) *
                            static_cast<double>(
                                in0[static_cast<size_t>((inStart + i) & kRingMask)]);
                }
                if (corr > bestCorr)
                {
                    bestCorr = corr;
                    bestOffset = offset;
                }
            }
            analysisMark = nextInputMark + bestOffset;
        }

        // Overlap sum of Hann grains of this length at this spacing is
        // (grainLength / spacing) * 0.5 on average; scale so it is
        // unity. (At the default spacing of one period in the
        // consumption direction this reduces to 2P / grainLength.)
        const float grainScale = static_cast<float>(
            juce::jlimit(0.25, 4.0, 2.0 * period * ratio / grainLength));

        const juce::int64 inStart = static_cast<juce::int64>(std::llround(analysisMark - half));
        const juce::int64 outStart = static_cast<juce::int64>(std::llround(nextInterPos - half));
        const int steps = static_cast<int>(std::llround(grainLength));
        // Grains accumulate into the ring (+=), so the territory a
        // grain is about to cover beyond everything written so far
        // must be zeroed first - otherwise content from one ring
        // period ago (16k samples) would bleed into the output when
        // the reader wraps around to those slots.
        if (outStart + steps > interValidTo)
        {
            for (int ch = 0; ch < numChannels; ++ch)
            {
                auto &out = interRings[static_cast<size_t>(ch)];
                for (juce::int64 p = interValidTo; p < outStart + steps; ++p)
                    out[static_cast<size_t>(p & kRingMask)] = 0.0f;
            }
        }
        for (int ch = 0; ch < numChannels; ++ch)
        {
            const auto &in = inputRings[static_cast<size_t>(ch)];
            auto &out = interRings[static_cast<size_t>(ch)];
            for (int i = 0; i < steps; ++i)
            {
                const double phase = static_cast<double>(i) / grainLength;
                const float w = static_cast<float>(
                    (0.5 - 0.5 * std::cos(2.0 * kPi * phase)) * grainScale);
                out[static_cast<size_t>((outStart + i) & kRingMask)] +=
                    in[static_cast<size_t>((inStart + i) & kRingMask)] * w;
            }
        }
        if (outStart < interValidFrom)
            interValidFrom = outStart;
        interValidTo = juce::jmax(interValidTo, outStart + steps);

        // Stretch by consumption rate: synthesis marks (in the
        // intermediate) advance period * ratio per grain while the
        // ideal analysis position advances one period, so the
        // intermediate runs ratio times longer than the input at the
        // same pitch. The reader stepping by ratio over it restores
        // the duration at ratio times the pitch.
        nextInputMark += period;
        nextInterPos += period * ratio;
    }
}

float StreamingPitchShifter::readRing(const std::vector<float> &ring,
                                      double position) const
{
    const auto base = static_cast<juce::int64>(std::floor(position));
    const float frac = static_cast<float>(position - static_cast<double>(base));
    const float a = ring[static_cast<size_t>(base & kRingMask)];
    const float b = ring[static_cast<size_t>((base + 1) & kRingMask)];
    return a + (b - a) * frac;
}
