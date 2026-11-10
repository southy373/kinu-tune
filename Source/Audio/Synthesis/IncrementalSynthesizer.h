#pragma once

#include "../../JuceHeader.h"
#include "../../Models/Project.h"
#include "../BigVGANVocoder.h"
#include "../SynthesisEngineType.h"
#include "../Vocoder.h"
#include "PitchShiftEngine.h"
#include "PsolaSynthesizer.h"
#include "RubberBandSynthesizer.h"
#include "SignalsmithStretchSynthesizer.h"
#include <atomic>
#include <mutex>
#include <string>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

/**
 * Voiced-Only Blend synthesizer.
 * Resynthesizes dirty regions using the selected engine, then blends:
 *   voiced frames  → synthesized audio
 *   unvoiced frames → original audio (preserves breathing)
 *
 * Two engines produce that synthesized audio. Vocoder regenerates the region
 * from its mel spectrogram; the classic path transforms the original waveform
 * instead. AI Resynthesis takes a third route to the same span: the selected
 * pitch-shift engine (PSOLA by default; Rubber Band or Signalsmith Stretch
 * since batch 4 - see PitchShiftEngine) first renders the edited pitch into
 * a scratch waveform and BigVGANVocoder regenerates that waveform from its
 * own mel, because BigVGAN has no F0 input to steer. The classic path uses
 * the same pitch-shift engine directly. Everything after the render -
 * blending, splicing, the audio move patches - is identical for all three.
 */
class IncrementalSynthesizer {
public:
  using ProgressCallback = std::function<void(const juce::String &message)>;
  using CompleteCallback = std::function<void(bool success)>;

  IncrementalSynthesizer();
  ~IncrementalSynthesizer();

  void setVocoder(Vocoder *v) { vocoder = v; }
  void setBigVganVocoder(BigVGANVocoder *v) { bigVganVocoder = v; }
  void setProject(Project *p) { project = p; }
  void setSynthesisEngine(SynthesisEngineType type) { synthesisEngine = type; }
  SynthesisEngineType getSynthesisEngine() const { return synthesisEngine; }

  // Which algorithm moves the pitch before the rebuild stage (batch
  // 4). Applies both to the classic path and to the scratch render the
  // BigVGAN path consumes; read at dispatch time, so a change lands on
  // the next render like an engine change does.
  void setPitchShiftEngine(PitchShiftEngine engine)
  {
    pitchShiftEngine = engine;
  }
  PitchShiftEngine getPitchShiftEngine() const { return pitchShiftEngine; }

  void synthesizeRegion(ProgressCallback onProgress,
                        CompleteCallback onComplete);

  void cancel();
  bool isSynthesizing() const { return isBusy.load(); }

  // Why the most recent engine render failed, when the engine reported
  // one (currently the BigVGAN path). Empty when it did not.
  std::string getLastSynthesisError() const;

private:
  /// Compute synthesis range: find voiced segments overlapping dirty range,
  /// expand to include complete segments + padding frames.
  std::pair<int, int> computeSynthesisRange(int dirtyStart, int dirtyEnd);

  /// Generate per-sample blend mask from voicedMask.
  /// 1.0 = use synthesized, 0.0 = use original, smooth ramps at transitions.
  std::vector<float> generateBlendMask(int startFrame, int endFrame,
                                       int hopSize);

  /// Per-output-frame map into the original audio: which source frame each
  /// rendered frame reads from. Identity unless a note has been retimed.
  /// Mirrors what applyNoteTimingToMel() does for the vocoder path.
  std::vector<double> buildSourceFrameMap(int startFrame, int endFrame) const;

  /// Per-output-frame target-over-source frequency ratio for the phase
  /// vocoder, given the already-retimed target F0 for the range.
  std::vector<float> buildPitchRatioCurve(int startFrame, int endFrame,
                                          const std::vector<double> &sourceFrameMap,
                                          const std::vector<float> &targetF0) const;

  void setLastSynthesisError(std::string error);

  // Run one request through the selected pitch-shift engine. Both
  // dispatch sites (classic output, BigVGAN scratch) go through here
  // so the choice can never apply to one path and not the other.
  void renderPitchShiftAsync(
      DspSynthesisRequest request,
      std::function<void(std::vector<float>)> onComplete,
      std::shared_ptr<std::atomic<bool>> cancelFlag);

  mutable std::mutex lastSynthesisErrorMutex;
  std::string lastSynthesisError;

  Vocoder *vocoder = nullptr;
  BigVGANVocoder *bigVganVocoder = nullptr;
  Project *project = nullptr;
  SynthesisEngineType synthesisEngine = SynthesisEngineType::Vocoder;
  PitchShiftEngine pitchShiftEngine = PitchShiftEngine::Psola;
  PsolaSynthesizer psola;
  RubberBandSynthesizer rubberBand;
  SignalsmithStretchSynthesizer signalsmithStretch;

  std::shared_ptr<std::atomic<bool>> cancelFlag;
  std::atomic<uint64_t> jobId{0};
  std::atomic<bool> isBusy{false};

  std::thread applyThread;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(IncrementalSynthesizer)
};
