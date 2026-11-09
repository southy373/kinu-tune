#pragma once

#include "../JuceHeader.h"

#include <vector>

/**
 * Batch 2 (insert prototype): a small, self-contained streaming pitch
 * shifter for the non-ARA insert mode. It is deliberately independent
 * of the offline synthesis path (PsolaSynthesizer renders whole
 * requests; formant::process is a block FFT shifter) so the prototype
 * can be removed without touching them.
 *
 * Method: TD-PSOLA time-stretch by the pitch ratio into an
 * intermediate stream, read back through a fractional (linear-interp)
 * resampler stepping by the ratio - the classic stretch-then-resample
 * pitch shift. Each grain's analysis centre is similarity-aligned
 * (cross-correlation against the intermediate tail, WSOLA-style);
 * without that, grains spaced period * ratio apart are phase-
 * incoherent in their overlap and the waveform tears. Formant
 * correction uses the PSOLA grain-length trick:
 * grains are 2 periods long at neutral, and scale by 2^(-st/12) for a
 * formant shift of st semitones (shorter grains read as higher
 * formants). F0 comes from an NSDF autocorrelation estimate over a
 * 1024-sample window every 256 samples (55-420 Hz); unvoiced stretches
 * use fixed nominal-period grains.
 *
 * Latency is fixed: getLatencySamples() (2048 samples, ~46 ms at
 * 44.1 kHz). All buffers are allocated in prepare(); process() never
 * allocates. At pitch 100% and neutral formant the output is the input
 * copied bit-exactly (the pipeline keeps running internally so
 * engaging a shift is seamless, but the dry signal is what leaves).
 */
class StreamingPitchShifter
{
public:
    static constexpr int kLatencySamples = 2048;

    void prepare(double sampleRate, int numChannels);
    void reset();

    /** Pitch as a percent: 100 = neutral. Range 50-200. */
    void setPitchPercent(float percent) { pitchPercent = percent; }
    /** Formant shift in semitones: 0 = neutral. Range -12..+12. */
    void setFormantShiftSemitones(float semitones) { formantSemitones = semitones; }

    int getLatencySamples() const { return kLatencySamples; }

    /** In-place processing; buffer holds input on entry, output on exit. */
    void process(juce::AudioBuffer<float> &buffer);

private:
    static constexpr int kRingSize = 16384; // power of two
    static constexpr int kRingMask = kRingSize - 1;
    static constexpr int kAnalysisWindow = 1024;
    static constexpr int kAnalysisHop = 256;

    void pushInput(const juce::AudioBuffer<float> &buffer);
    void runAnalysis();
    void runSynthesis();
    float estimatePeriodSamples();
    float readRing(const std::vector<float> &ring, double position) const;

    double sampleRate = 44100.0;
    int numChannels = 0;
    float pitchPercent = 100.0f;
    float formantSemitones = 0.0f;

    // Per-channel rings (input history and intermediate stretch stream),
    // plus a mono ring for analysis. Absolute sample indices map into
    // the rings with the mask, so no data is ever moved.
    std::vector<std::vector<float>> inputRings;
    std::vector<std::vector<float>> interRings;
    std::vector<float> monoRing;
    std::vector<float> analysisScratch;
    std::vector<float> nsdfScratch;

    juce::int64 inputWritten = 0;   // absolute count of input samples
    juce::int64 nextAnalysisEnd = 0; // next window end to analyse
    double currentPeriod = 160.0;    // latest period estimate (samples)
    bool voiced = false;

    double nextInputMark = 0.0;  // input position of the next grain centre
    double nextInterPos = 0.0;   // intermediate position of that centre
    juce::int64 interValidFrom = 0;
    juce::int64 interValidTo = 0;
    double readerPos = 0.0;      // intermediate position of the next output sample
    // The grain chain and readerPos are seeded together on the first
    // process() call, from the ratio in effect at that moment - not in
    // reset(), where a stale percent would skew the effective latency.
    bool streamSeeded = false;
    juce::int64 outputWritten = 0;
};
