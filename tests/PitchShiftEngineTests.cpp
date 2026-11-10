// Batch 4: the two added pitch-shift engines (Rubber Band R3 and
// Signalsmith Stretch) render a real, non-silent signal through the
// shared DspSynthesisRequest contract.
//
// For each engine and each ratio in {0.5, 1.0, 2.0} the render must
// come back at exactly the requested length, stay finite, keep a sane
// energy (RMS in the steady middle within a generous band of the
// source's), and actually move the fundamental to ratio x source F0.
// A stretched time map (slope 2) must still produce the exact output
// length at the unshifted fundamental.
#include "../Source/Audio/Synthesis/PitchShiftEngine.h"
#include "../Source/Audio/Synthesis/RubberBandSynthesizer.h"
#include "../Source/Audio/Synthesis/SignalsmithStretchSynthesizer.h"
#include "TestAssert.h"

#include <cmath>
#include <iostream>
#include <vector>

namespace
{

constexpr int kSampleRate = 44100;
constexpr int kHop = 512;
constexpr double kSourceF0 = 110.0;

// A harmonic stack (a sawtooth-like voice surrogate): the fundamental
// plus 12 harmonics at 1/h. A single sine would be a misleading test
// signal for formant-preserving engines - its whole spectrum is the
// formant - so the checks use this instead.
float harmonicSample(long absoluteSample)
{
  double sum = 0.0;
  for (int h = 1; h <= 12; ++h)
    sum += std::sin(2.0 * M_PI * kSourceF0 * h *
                    static_cast<double>(absoluteSample) / kSampleRate) /
           h;
  return static_cast<float>(0.25 * sum);
}

double middleRms(const std::vector<float> &samples)
{
  const size_t begin = samples.size() / 4;
  const size_t end = samples.size() * 3 / 4;
  double energy = 0.0;
  for (size_t i = begin; i < end; ++i)
    energy += static_cast<double>(samples[i]) * samples[i];
  return std::sqrt(energy / static_cast<double>(end - begin));
}

// Fundamental estimate by normalised autocorrelation over the steady
// middle, searched over 50..500 Hz.
double estimateF0(const std::vector<float> &samples)
{
  const int begin = static_cast<int>(samples.size()) / 4;
  const int length = static_cast<int>(samples.size()) / 2;
  const int minLag = kSampleRate / 500;
  const int maxLag = kSampleRate / 50;
  double best = -1.0;
  int bestLag = minLag;
  for (int lag = minLag; lag <= maxLag; ++lag)
  {
    double dot = 0.0, a = 0.0, b = 0.0;
    for (int i = 0; i < length; ++i)
    {
      const double x = samples[static_cast<size_t>(begin + i)];
      const double y = samples[static_cast<size_t>(begin + i + lag)];
      dot += x * y;
      a += x * x;
      b += y * y;
    }
    const double score = dot / std::sqrt(std::max(1e-12, a * b));
    if (score > best)
    {
      best = score;
      bestLag = lag;
    }
  }
  return static_cast<double>(kSampleRate) / bestLag;
}

// A request whose slice starts 8 frames before the render range, like
// IncrementalSynthesizer's padded slices. `slope` is the time map's
// source-frames-per-output-frame; ratio is the constant pitch ratio.
DspSynthesisRequest makeRequest(int numFrames, double slope, float ratio)
{
  constexpr int kSliceStartFrame = 96;
  constexpr int kPadFrames = 8;
  const int sourceFrames =
      kPadFrames + static_cast<int>(std::ceil(numFrames * slope)) + kPadFrames;

  DspSynthesisRequest request;
  request.sourceStartSample = kSliceStartFrame * kHop;
  request.source.resize(static_cast<size_t>(sourceFrames * kHop));
  for (size_t i = 0; i < request.source.size(); ++i)
    request.source[i] = harmonicSample(request.sourceStartSample +
                                       static_cast<long>(i));
  request.sourceFrame.resize(static_cast<size_t>(numFrames));
  request.pitchRatio.assign(static_cast<size_t>(numFrames), ratio);
  for (int f = 0; f < numFrames; ++f)
    request.sourceFrame[static_cast<size_t>(f)] =
        kSliceStartFrame + kPadFrames + slope * f;
  const int absoluteFrames = kSliceStartFrame + sourceFrames + 8;
  request.sourceF0.assign(static_cast<size_t>(absoluteFrames),
                          static_cast<float>(kSourceF0));
  request.sourceVoiced.assign(static_cast<size_t>(absoluteFrames), 1);
  request.hopSize = kHop;
  request.numOutputFrames = numFrames;
  request.sampleRate = kSampleRate;
  request.preserveFormants = true;
  return request;
}

void checkRender(const char *engineName, const std::vector<float> &output,
                 const std::vector<float> &source, int numFrames,
                 double expectedF0)
{
  CHECK(output.size() == static_cast<size_t>(numFrames * kHop));
  for (float v : output)
    CHECK(std::isfinite(v));
  const double sourceRms = middleRms(source);
  const double outputRms = middleRms(output);
  std::cout << engineName << ": rms source=" << sourceRms
            << " output=" << outputRms << "\n";
  CHECK(outputRms > 0.45 * sourceRms);
  CHECK(outputRms < 1.8 * sourceRms);
  const double f0 = estimateF0(output);
  std::cout << engineName << ": f0=" << f0 << " expected=" << expectedF0
            << "\n";
  CHECK(std::abs(f0 - expectedF0) < 0.10 * expectedF0);
}

} // namespace

int main()
{
  // The persisted strings round-trip, and anything unknown (including
  // a missing value from an older settings file) means PSOLA.
  CHECK(pitchShiftEngineToString(PitchShiftEngine::Psola) == "psola");
  CHECK(pitchShiftEngineToString(PitchShiftEngine::RubberBand) ==
        "rubberband");
  CHECK(pitchShiftEngineToString(PitchShiftEngine::SignalsmithStretch) ==
        "signalsmith");
  CHECK(stringToPitchShiftEngine("rubberband") ==
        PitchShiftEngine::RubberBand);
  CHECK(stringToPitchShiftEngine("signalsmith") ==
        PitchShiftEngine::SignalsmithStretch);
  CHECK(stringToPitchShiftEngine("psola") == PitchShiftEngine::Psola);
  CHECK(stringToPitchShiftEngine("") == PitchShiftEngine::Psola);
  CHECK(stringToPitchShiftEngine("nonsense") == PitchShiftEngine::Psola);

  constexpr int kFrames = 87; // ~1 second
  for (float ratio : {0.5f, 1.0f, 2.0f})
  {
    const auto request = makeRequest(kFrames, 1.0, ratio);
    // The reference energy comes from the source slice's own middle.
    checkRender("RubberBand", RubberBandSynthesizer::render(request),
                request.source, kFrames, kSourceF0 * ratio);
    checkRender("Signalsmith",
                SignalsmithStretchSynthesizer::render(request),
                request.source, kFrames, kSourceF0 * ratio);
  }

  // A stretched time map: the output length is the output frame count
  // regardless of how much source the map consumes.
  for (int engine = 0; engine < 2; ++engine)
  {
    const auto request = makeRequest(kFrames, 2.0, 1.0f);
    const auto output = engine == 0
                            ? RubberBandSynthesizer::render(request)
                            : SignalsmithStretchSynthesizer::render(request);
    checkRender(engine == 0 ? "RubberBand slope2" : "Signalsmith slope2",
                output, request.source, kFrames, kSourceF0);
  }

  // A pre-cancelled render returns empty, like PSOLA's does.
  auto cancelFlag = std::make_shared<std::atomic<bool>>(true);
  const auto request = makeRequest(kFrames, 1.0, 1.0f);
  CHECK(RubberBandSynthesizer::render(request, cancelFlag).empty());
  CHECK(SignalsmithStretchSynthesizer::render(request, cancelFlag).empty());

  std::cout << "PitchShiftEngineTests passed\n";
  return 0;
}
