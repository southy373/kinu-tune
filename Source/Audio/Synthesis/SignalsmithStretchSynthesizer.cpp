// Kinu Tune - Copyright (C) 2026 Keinoaza
// Part of Kinu Tune, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.

#include "SignalsmithStretchSynthesizer.h"

#include "AsyncDspRenderWorker.h"

#include <signalsmith-stretch.h>

#include <algorithm>
#include <cmath>

namespace
{

AsyncDspRenderWorker &signalsmithWorker()
{
  // One worker for the whole process, as with PSOLA's.
  static AsyncDspRenderWorker worker(
      [](const DspSynthesisRequest &request,
         const std::shared_ptr<std::atomic<bool>> &cancelFlag) {
        return SignalsmithStretchSynthesizer::render(request, cancelFlag);
      });
  return worker;
}

bool isCancelled(const std::shared_ptr<std::atomic<bool>> &flag)
{
  return flag != nullptr && flag->load();
}

// The Signalsmith IO adapters hand the library a raw pointer per
// channel; the render is mono, so channel 0 is the block buffer.
struct MonoInput
{
  const float *samples = nullptr;
  const float *operator[](int) const { return samples; }
};
struct MonoOutput
{
  float *samples = nullptr;
  float *operator[](int) const { return samples; }
};

} // namespace

SignalsmithStretchSynthesizer::SignalsmithStretchSynthesizer() = default;
SignalsmithStretchSynthesizer::~SignalsmithStretchSynthesizer() = default;

void SignalsmithStretchSynthesizer::renderAsync(
    Request request, std::function<void(std::vector<float>)> callback,
    std::shared_ptr<std::atomic<bool>> cancelFlag)
{
  signalsmithWorker().post(std::move(request), std::move(callback),
                           std::move(cancelFlag));
}

std::vector<float> SignalsmithStretchSynthesizer::render(
    const Request &request,
    const std::shared_ptr<std::atomic<bool>> &cancelFlag)
{
  const int numOutputFrames = request.numOutputFrames;
  const int hopSize = request.hopSize;
  if (numOutputFrames <= 0 || hopSize <= 0 || request.source.empty())
    return {};
  if (static_cast<int>(request.sourceFrame.size()) < numOutputFrames ||
      static_cast<int>(request.pitchRatio.size()) < numOutputFrames)
    return {};

  const int expectedSamples = numOutputFrames * hopSize;
  const int sourceLength = static_cast<int>(request.source.size());
  const int sampleRate = request.sampleRate > 0 ? request.sampleRate : 44100;

  // Slice-relative source position for an output sample position, the
  // same mapping PSOLA uses.
  auto sourcePosAt = [&](double outputSample) {
    return dspSynthesis::sampleSourceFrame(
               request.sourceFrame, outputSample / hopSize,
               numOutputFrames) *
               static_cast<double>(hopSize) -
           static_cast<double>(request.sourceStartSample);
  };
  auto readSource = [&](long index) -> float {
    return (index >= 0 && index < sourceLength)
               ? request.source[static_cast<size_t>(index)]
               : 0.0f;
  };

  signalsmith::stretch::SignalsmithStretch<float> stretch;
  stretch.presetDefault(1, static_cast<float>(sampleRate));
  stretch.reset();
  // The formant factor does not depend on the ratio, only on the
  // request's preserve flag, so it is set once for the whole render.
  if (request.preserveFormants)
    stretch.setFormantFactor(1.0f, true);
  else
    stretch.setFormantFactor(1.0f, false);

  // The input->output delay of the default preset, measured against
  // the vendored 1.4.0 header: inputLatency() + outputLatency()
  // (= blockSamples()). The first that-many output samples are
  // pre-roll synthesised from the seek history and are discarded.
  const long latency =
      static_cast<long>(stretch.inputLatency()) + stretch.outputLatency();

  // Prime the analysis with the real input history ending where the
  // render starts, so the first kept sample is already steady-state.
  const double startPos = sourcePosAt(0.0);
  const long startIndex = std::llround(startPos);
  const int historyLength = static_cast<int>(
      std::min<long>(stretch.seekLength(), std::max<long>(0, startIndex)));
  std::vector<float> history(static_cast<size_t>(std::max(1, historyLength)),
                             0.0f);
  for (int i = 0; i < historyLength; ++i)
    history[static_cast<size_t>(i)] =
        readSource(startIndex - historyLength + i);
  const double initialSlope = sourcePosAt(1.0 * hopSize) - sourcePosAt(0.0);
  stretch.seek(MonoInput{history.data()}, historyLength,
               std::clamp(initialSlope / hopSize, 0.05, 20.0));

  std::vector<float> result;
  result.reserve(static_cast<size_t>(expectedSamples));
  std::vector<float> blockInput;
  std::vector<float> blockOutput(1024, 0.0f);

  // The input cursor advances by the map's movement over each block's
  // output span, in output-stream coordinates that start at -latency:
  // the seek history accounts for everything before the start point,
  // so feeding begins at the start point immediately and the pre-roll
  // blocks still consume their share of input.
  double cursor = startPos;
  double blockStartOut = -static_cast<double>(latency);
  long produced = 0;
  const long totalToProduce = static_cast<long>(expectedSamples) + latency;
  while (produced < totalToProduce)
  {
    if (isCancelled(cancelFlag))
      return {};
    const int blockLength =
        static_cast<int>(std::min<long>(1024, totalToProduce - produced));
    const double blockEndOut = blockStartOut + blockLength;
    const double target =
        cursor + (sourcePosAt(blockEndOut) - sourcePosAt(blockStartOut));
    const long inStart = std::llround(cursor);
    // The map is monotonic for every real edit; if a pathological map
    // ever ran backwards, hold the cursor instead of re-feeding audio.
    const bool advance = target > cursor;
    const long inEnd = advance ? std::llround(target) : inStart;
    const int inLength = static_cast<int>(std::max<long>(0, inEnd - inStart));
    blockInput.assign(static_cast<size_t>(std::max(1, inLength)), 0.0f);
    for (int i = 0; i < inLength; ++i)
      blockInput[static_cast<size_t>(i)] = readSource(inStart + i);

    const double ratio = dspSynthesis::samplePitchRatio(
        request.pitchRatio,
        std::max(0.0, (blockStartOut + blockEndOut) * 0.5 / hopSize),
        numOutputFrames);
    stretch.setTransposeFactor(static_cast<float>(std::clamp(
        ratio, static_cast<double>(0.25f), static_cast<double>(4.0f))));
    stretch.process(MonoInput{blockInput.data()}, inLength,
                    MonoOutput{blockOutput.data()}, blockLength);

    if (advance)
      cursor = target;
    blockStartOut = blockEndOut;
    for (int i = 0; i < blockLength; ++i)
    {
      if (produced + i >= latency &&
          static_cast<int>(result.size()) < expectedSamples)
        result.push_back(blockOutput[static_cast<size_t>(i)]);
    }
    produced += blockLength;
  }
  result.resize(static_cast<size_t>(expectedSamples), 0.0f);
  return result;
}
