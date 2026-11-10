// Kinu Tune - Copyright (C) 2026 Keinoaza
// Part of Kinu Tune, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.

#include "RubberBandSynthesizer.h"

#include "AsyncDspRenderWorker.h"

#include <rubberband/RubberBandStretcher.h>

#include <algorithm>
#include <cmath>

namespace
{

AsyncDspRenderWorker &rubberBandWorker()
{
  // One worker for the whole process, as with PSOLA's.
  static AsyncDspRenderWorker worker(
      [](const DspSynthesisRequest &request,
         const std::shared_ptr<std::atomic<bool>> &cancelFlag) {
        return RubberBandSynthesizer::render(request, cancelFlag);
      });
  return worker;
}

bool isCancelled(const std::shared_ptr<std::atomic<bool>> &flag)
{
  return flag != nullptr && flag->load();
}

// Render one constant-ratio segment with a fresh offline stretcher.
// inStart/inLength address the request's source slice; the result is
// resized to exactly outLength samples.
std::vector<float> renderSegment(const DspSynthesisRequest &request,
                                 int inStart, int inLength, double pitchScale,
                                 int outLength)
{
  std::vector<float> result(static_cast<size_t>(std::max(0, outLength)), 0.0f);
  if (inLength <= 0 || outLength <= 0)
    return result;

  const int sampleRate = request.sampleRate > 0 ? request.sampleRate : 44100;

  RubberBand::RubberBandStretcher stretcher(
      static_cast<size_t>(sampleRate), 1,
      RubberBand::RubberBandStretcher::OptionProcessOffline |
          RubberBand::RubberBandStretcher::OptionEngineFiner |
          RubberBand::RubberBandStretcher::OptionChannelsTogether |
          RubberBand::RubberBandStretcher::OptionPitchHighQuality |
          RubberBand::RubberBandStretcher::OptionFormantPreserved |
          // Renders are serialised on this engine's one worker thread;
          // the stretcher must not fan out to more threads of its own.
          RubberBand::RubberBandStretcher::OptionThreadingNever);

  const double clampedPitch = std::clamp(pitchScale, 0.25, 4.0);
  stretcher.setPitchScale(clampedPitch);
  stretcher.setTimeRatio(static_cast<double>(outLength) /
                         static_cast<double>(inLength));
  // Formant handling mirrors PSOLA, which leaves the envelope in
  // place: with preserveFormants the envelope's total shift is 1.0
  // (the explicit reciprocal of the pitch scale); without it the
  // envelope moves with the pitch. The user's own formant amount is
  // applied later by IncrementalSynthesizer and never reaches here.
  stretcher.setFormantScale(request.preserveFormants ? 1.0 / clampedPitch
                                                     : 1.0);
  stretcher.setExpectedInputDuration(static_cast<size_t>(inLength));

  const float *input = request.source.data() + inStart;
  stretcher.study(&input, static_cast<size_t>(inLength), true);
  stretcher.setMaxProcessSize(static_cast<size_t>(inLength));
  stretcher.process(&input, static_cast<size_t>(inLength), true);

  int written = 0;
  while (written < outLength)
  {
    const int avail = stretcher.available();
    if (avail <= 0)
      break;
    float *dest = result.data() + written;
    const int got = stretcher.retrieve(
        &dest, static_cast<size_t>(std::min(avail, outLength - written)));
    if (got <= 0)
      break;
    written += got;
  }
  // Rubber Band's offline output length can land a few samples either
  // side of the nominal ratio. A shortfall keeps the tail at the last
  // produced value instead of dropping to zero mid-crossfade.
  if (written > 0 && written < outLength)
    std::fill(result.begin() + written, result.end(),
              result[static_cast<size_t>(written - 1)]);
  return result;
}

} // namespace

RubberBandSynthesizer::RubberBandSynthesizer() = default;
RubberBandSynthesizer::~RubberBandSynthesizer() = default;

void RubberBandSynthesizer::renderAsync(
    Request request, std::function<void(std::vector<float>)> callback,
    std::shared_ptr<std::atomic<bool>> cancelFlag)
{
  rubberBandWorker().post(std::move(request), std::move(callback),
                          std::move(cancelFlag));
}

std::vector<float> RubberBandSynthesizer::render(
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

  // Slice-relative source position for an output frame, the same
  // mapping PSOLA uses.
  auto sourcePosAt = [&](double outputFrame) {
    return dspSynthesis::sampleSourceFrame(request.sourceFrame, outputFrame,
                                           numOutputFrames) *
               static_cast<double>(hopSize) -
           static_cast<double>(request.sourceStartSample);
  };

  // Segment boundaries: a new segment starts wherever the pitch ratio
  // drifts more than 0.2% from the segment's starting ratio, wherever
  // the source-frame map's slope drifts more than 0.5%, and a boundary
  // is flagged as a jump wherever the map moves by more than a frame
  // in one step (a retimed note edge - the input is not contiguous
  // across it). bounds[k] is the first frame of segment k and
  // joinIsJump[k] describes the join at bounds[k].
  std::vector<int> bounds{0};
  std::vector<bool> joinIsJump{false};
  {
    double segmentRatio = dspSynthesis::samplePitchRatio(
        request.pitchRatio, 0.0, numOutputFrames);
    double segmentSlope = 0.0;
    bool haveSlope = false;
    for (int frame = 1; frame < numOutputFrames; ++frame)
    {
      const double ratio = dspSynthesis::samplePitchRatio(
          request.pitchRatio, frame, numOutputFrames);
      const double slope =
          dspSynthesis::sampleSourceFrame(request.sourceFrame, frame,
                                          numOutputFrames) -
          dspSynthesis::sampleSourceFrame(request.sourceFrame, frame - 1,
                                          numOutputFrames);
      if (!haveSlope)
      {
        segmentSlope = slope;
        haveSlope = true;
      }
      const bool ratioDrift =
          std::abs(ratio - segmentRatio) > 0.002 * segmentRatio;
      const bool slopeDrift = std::abs(slope - segmentSlope) >
                              0.005 * std::max(0.05, segmentSlope);
      const bool jump = slopeDrift && std::abs(slope - segmentSlope) > 1.0;
      if (ratioDrift || slopeDrift)
      {
        bounds.push_back(frame);
        joinIsJump.push_back(jump);
        segmentRatio = ratio;
        // After a jump the slope at this frame is the jump itself; the
        // new segment's reference slope is the following frame's.
        if (jump && frame + 1 < numOutputFrames)
          segmentSlope =
              dspSynthesis::sampleSourceFrame(request.sourceFrame, frame + 1,
                                              numOutputFrames) -
              dspSynthesis::sampleSourceFrame(request.sourceFrame, frame,
                                              numOutputFrames);
        else
          segmentSlope = slope;
      }
    }
  }
  bounds.push_back(numOutputFrames);
  joinIsJump.push_back(false);

  // Merge segments shorter than six output frames into a neighbour:
  // a fresh offline stretcher per sliver wastes its study pass and can
  // sound worse than a slightly averaged ratio. The merged segment
  // keeps the join flags of its outer edges.
  for (bool merged = true; merged && bounds.size() > 2;)
  {
    merged = false;
    for (size_t i = 1; i + 1 < bounds.size(); ++i)
    {
      if (bounds[i + 1] - bounds[i] < 6)
      {
        bounds.erase(bounds.begin() + static_cast<long>(i + 1));
        joinIsJump.erase(joinIsJump.begin() + static_cast<long>(i + 1));
        merged = true;
        break;
      }
    }
  }

  constexpr int kPadFrames = 1;
  std::vector<float> accum(static_cast<size_t>(expectedSamples), 0.0f);
  std::vector<float> weights(static_cast<size_t>(expectedSamples), 0.0f);

  for (size_t seg = 0; seg + 1 < bounds.size(); ++seg)
  {
    if (isCancelled(cancelFlag))
      return {};
    const int coreStart = bounds[seg];
    const int coreEnd = bounds[seg + 1];
    // No extension across a jump join: the audio either side is from
    // somewhere else entirely.
    const int extStart = (seg == 0 || joinIsJump[seg])
                             ? coreStart
                             : std::max(0, coreStart - kPadFrames);
    const int extEnd = (seg + 2 == bounds.size() || joinIsJump[seg + 1])
                           ? coreEnd
                           : std::min(numOutputFrames, coreEnd + kPadFrames);
    if (extEnd <= extStart)
      continue;

    const int inStart = static_cast<int>(std::clamp<long>(
        std::llround(sourcePosAt(extStart)), 0, sourceLength));
    const int inEnd = static_cast<int>(std::clamp<long>(
        std::llround(sourcePosAt(extEnd)), inStart, sourceLength));
    const double pitchScale = dspSynthesis::samplePitchRatio(
        request.pitchRatio, (extStart + extEnd) / 2.0, numOutputFrames);
    const int outLength = (extEnd - extStart) * hopSize;

    const auto segment = renderSegment(request, inStart, inEnd - inStart,
                                       pitchScale, outLength);

    const int destStart = extStart * hopSize;
    for (int i = 0; i < outLength; ++i)
    {
      const int outFrame = extStart + i / hopSize;
      float weight = 1.0f;
      if (outFrame < coreStart)
        weight = static_cast<float>(i) / static_cast<float>(std::max(
                     1, (coreStart - extStart) * hopSize));
      else if (outFrame >= coreEnd)
        weight =
            1.0f - static_cast<float>(i - (coreEnd - extStart) * hopSize) /
                       static_cast<float>(
                           std::max(1, (extEnd - coreEnd) * hopSize));
      const size_t at = static_cast<size_t>(destStart + i);
      accum[at] += weight * segment[static_cast<size_t>(i)];
      weights[at] += weight;
    }
  }

  std::vector<float> result(static_cast<size_t>(expectedSamples), 0.0f);
  for (int i = 0; i < expectedSamples; ++i)
  {
    const float w = weights[static_cast<size_t>(i)];
    if (w > 0.0f)
      result[static_cast<size_t>(i)] = accum[static_cast<size_t>(i)] / w;
  }
  return result;
}
