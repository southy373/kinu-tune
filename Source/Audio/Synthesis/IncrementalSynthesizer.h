#pragma once

#include "../../JuceHeader.h"
#include "../../Models/Project.h"
#include "../BigVGANVocoder.h"
#include "../SynthesisEngineType.h"
#include "../Vocoder.h"
#include "PsolaSynthesizer.h"
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
 * from its mel spectrogram; PsolaSynthesizer transforms the original waveform
 * instead. AI Resynthesis takes a third route to the same span: PSOLA first
 * renders the edited pitch into a scratch waveform and BigVGANVocoder
 * regenerates that waveform from its own mel, because BigVGAN has no F0
 * input to steer. Everything after the render - blending, splicing, the
 * audio move patches - is identical for all three.
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

  mutable std::mutex lastSynthesisErrorMutex;
  std::string lastSynthesisError;

  Vocoder *vocoder = nullptr;
  BigVGANVocoder *bigVganVocoder = nullptr;
  Project *project = nullptr;
  SynthesisEngineType synthesisEngine = SynthesisEngineType::Vocoder;
  PsolaSynthesizer psola;

  std::shared_ptr<std::atomic<bool>> cancelFlag;
  std::atomic<uint64_t> jobId{0};
  std::atomic<bool> isBusy{false};

  std::thread applyThread;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(IncrementalSynthesizer)
};
