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
 * DspSynthesisRequest renderer backed by Signalsmith Stretch, as an
 * alternative pitch-shift stage to PSOLA (batch 4; see
 * PitchShiftEngine). Takes the same request and returns the same span
 * of samples as PsolaSynthesizer, so the two are interchangeable to
 * the caller.
 *
 * Signalsmith is a streaming phase vocoder: render() drives it block
 * by block, feeding input so the read position follows the request's
 * source-frame map and setting the transpose factor from the pitch
 * ratio curve as it goes. The library's input+output latency
 * (inputLatency() + outputLatency() = blockSamples() at the default
 * preset, measured against the vendored 1.4.0) is discarded as
 * pre-roll after a seek() primed with the real input history, which
 * keeps the output sample-aligned with the map exactly as PSOLA's is;
 * the alignment is covered by tests/PitchShiftEngineTests.cpp.
 *
 * Formants: preserveFormants maps onto setFormantFactor(1.0,
 * compensatePitch=true), which holds the formant envelope at the
 * source pitch like PSOLA does; the plug-in's own formant shifting
 * runs afterwards, in IncrementalSynthesizer, unchanged.
 */
class SignalsmithStretchSynthesizer
{
public:
  using Request = DspSynthesisRequest;

  SignalsmithStretchSynthesizer();
  ~SignalsmithStretchSynthesizer();

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
  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(
      SignalsmithStretchSynthesizer)
};
