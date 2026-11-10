// Kinu Tune - Copyright (C) 2026 Keinoaza
// Part of Kinu Tune, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.
#pragma once

#include "../../JuceHeader.h"
#include "DspSynthesisRequest.h"

#include <atomic>
#include <functional>
#include <memory>
#include <vector>

/**
 * DspSynthesisRequest renderer backed by the Rubber Band Library's R3
 * ("Finer") engine, as an alternative pitch-shift stage to PSOLA
 * (batch 4; see PitchShiftEngine). Takes the same request and returns
 * the same span of samples as PsolaSynthesizer, so the two are
 * interchangeable to the caller.
 *
 * Rubber Band's offline mode wants one fixed pitch scale and time
 * ratio per stretcher, while a request carries curves (the pitch ratio
 * and the source-frame map both vary per output frame). render()
 * therefore splits the request into segments over which both curves
 * are effectively constant, renders each segment with its own offline
 * stretcher extended by one hop on either side, and crossfades the
 * extensions so segment joins do not click. A map discontinuity (a
 * retimed note edge) gets no extension across it: the audio either
 * side comes from somewhere else entirely.
 *
 * Formants: the request's preserveFormants maps onto Rubber Band's
 * OptionFormantPreserved plus an explicit setFormantScale, so the
 * pitch stage leaves the formant envelope where PSOLA leaves it (the
 * plug-in's own formant shifting runs afterwards, in
 * IncrementalSynthesizer, unchanged).
 */
class RubberBandSynthesizer
{
public:
  using Request = DspSynthesisRequest;

  RubberBandSynthesizer();
  ~RubberBandSynthesizer();

  /**
   * Render on the engine's shared worker thread and deliver the
   * result to @p callback. The callback receives an empty vector on
   * failure or cancellation.
   */
  void renderAsync(Request request,
                   std::function<void(std::vector<float>)> callback,
                   std::shared_ptr<std::atomic<bool>> cancelFlag = nullptr);

  /// Synchronous render. Returns empty on invalid input or cancellation.
  static std::vector<float> render(
      const Request &request,
      const std::shared_ptr<std::atomic<bool>> &cancelFlag = nullptr);

private:
  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RubberBandSynthesizer)
};
