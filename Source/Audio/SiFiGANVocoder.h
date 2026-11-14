// Kinu Tune - Copyright (C) 2026 Keinoaza
// Part of Kinu Tune, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.

#pragma once

#include "../JuceHeader.h"
#include "SiFiGANFeatures.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef HAVE_ONNXRUNTIME
#include <onnxruntime_cxx_api.h>
#ifdef USE_DIRECTML
#include <dml_provider_factory.h>
#endif
#endif

/**
 * SiFi-GAN neural vocoder wrapper, ONNX Runtime (Plan B route).
 *
 * Unlike BigVGAN, SiFi-GAN takes the edited pitch as data: the caller
 * hands over the original waveform plus the source and edited F0
 * tracks, and this class builds the model's actual inputs on its
 * worker thread - the WORLD-based features of the original audio
 * (SiFiGANFeatures), the continuous edited F0, the sine excitation and
 * the dilated factors - and runs the generator. No pitch-shifted
 * scratch waveform is involved.
 *
 * The model is the project's own training output (24kHz, hop 120,
 * 43-dim mcep+bap features), small enough (about 35MB) to sit next to
 * the other models; it only ever runs on the background re-render
 * path. Session establishment mirrors BigVGANVocoder's ladder
 * (DirectML rungs, then CPU, each adopted only after a probe inference
 * succeeds), because this graph also contains ConvTranspose and Gather
 * nodes a DirectML session can accept at creation and fail on at
 * inference. Chunked inference slices pre-built input tensors, so the
 * excitation phase - a cumulative sum - is continuous across chunks
 * by construction.
 *
 * The model file (sifigan_115k_gather.onnx) and its feature statistics
 * sidecar (sifigan_kokoro_stats.json) are a pair: the statistics belong
 * to the training run that produced the weights, so loadModel takes
 * both and refuses a model without usable statistics.
 */
class SiFiGANVocoder
{
public:
  SiFiGANVocoder();
  ~SiFiGANVocoder();

  // Stop accepting work and let active inference exit at its next chunk.
  void requestShutdown();

  /**
   * Load the SiFi-GAN model and its statistics sidecar.
   * @param modelPath Path to sifigan_115k_gather.onnx
   * @param statsPath Path to sifigan_kokoro_stats.json (mcep/bap scalers)
   * @return true if successful
   */
  bool loadModel(const juce::File &modelPath, const juce::File &statsPath);

  bool isLoaded() const { return loaded; }

  /**
   * Whether the loaded session runs through a GPU execution provider
   * (DirectML). False before a successful load and after any CPU fallback.
   */
  bool isUsingGpu() const { return usingGpuProvider; }

  /**
   * Execution provider of the loaded session: "DirectML" or "CPU".
   */
  std::string activeProviderName() const { return activeProvider; }

  static bool isOnnxRuntimeAvailable();

  /**
   * Why the most recent loadModel() or infer() failed, in plain text
   * (empty when nothing has failed yet). Surfaced in the editor's render
   * status so a failed render says why instead of just failing.
   */
  std::string getLastError() const { return lastError; }

  /**
   * Everything one render needs, on the caller's grids:
   * - sourceWaveform: the original audio span (mono, sourceSampleRate),
   *   starting at the analysis frame the other arrays are aligned to.
   * - sourceF0: the analysed (unedited) F0 per analysis frame, aligned
   *   to sourceWaveform's first frame; used for the source features.
   * - targetF0: the edited F0 per output analysis frame.
   * - sourceFrameMap: per output analysis frame, the source analysis
   *   frame it reads from, RELATIVE to the span start (identity when
   *   nothing was retimed). The feature frames follow this map, like
   *   the mel of the NSF vocoder path does.
   * - numOutputFrames: the render length in analysis frames; the
   *   result is that many analysisHop samples at sourceSampleRate.
   */
  struct RenderRequest
  {
    std::vector<float> sourceWaveform;
    int sourceSampleRate = 44100;
    int analysisHop = 512;
    std::vector<float> sourceF0;
    std::vector<float> targetF0;
    std::vector<double> sourceFrameMap;
    int numOutputFrames = 0;
  };

  /**
   * Asynchronous waveform-to-waveform resynthesis (see RenderRequest).
   * The callback receives the rendered span at the request's sample
   * rate, or an empty vector on failure/cancellation.
   */
  void inferFromWaveformAsync(
      RenderRequest request,
      std::function<void(std::vector<float>)> callback,
      std::shared_ptr<std::atomic<bool>> cancelFlag = nullptr);

  /**
   * Synchronous render of one request (the async path's worker body).
   * Exposed for the model inference tests.
   */
  std::vector<float> inferSync(
      const RenderRequest &request,
      std::shared_ptr<std::atomic<bool>> cancelFlag = nullptr);

  /**
   * Synchronous generator run from pre-built features: @p cf0 is the
   * continuous F0 [T] and @p normalisedC the normalised features
   * [T][43]; the excitation and dilated factors are built here (fixed
   * seed, so a render is reproducible). Returns T * 120 samples at
   * 24kHz, or empty on failure. Exposed for the model inference tests.
   */
  std::vector<float> infer(
      const std::vector<float> &cf0,
      const std::vector<std::vector<float>> &normalisedC,
      std::shared_ptr<std::atomic<bool>> cancelFlag = nullptr);

  int getSampleRate() const { return sifigan::kSampleRate; }
  int getHopSize() const { return sifigan::kHopSize; }

private:
  struct AsyncTask
  {
    RenderRequest request;
    std::function<void(std::vector<float>)> callback;
    std::shared_ptr<std::atomic<bool>> cancelFlag;
  };

  bool loaded = false;
  bool usingGpuProvider = false;
  std::string activeProvider = "CPU";

  juce::File modelFile;
  juce::File activeModelFile;
  std::string lastError;
  sifigan::Stats stats;
  std::unique_ptr<std::ofstream> logFile;

  std::atomic<bool> isShuttingDown{false};
  std::atomic<int> activeAsyncTasks{0};
  std::mutex asyncMutex;
  std::condition_variable asyncCondition;
  std::thread asyncWorker;
  std::deque<AsyncTask> asyncQueue;

  // Mutex to protect ONNX session access during inference
  mutable std::mutex inferenceMutex;

  void log(const std::string &message);
  void enqueueTask(AsyncTask task);
  std::vector<float> runTask(AsyncTask &task);

#ifdef HAVE_ONNXRUNTIME
  std::unique_ptr<Ort::Env> onnxEnv;
  std::unique_ptr<Ort::Session> onnxSession;
  std::unique_ptr<Ort::AllocatorWithDefaultOptions> allocator;

  std::vector<const char *> inputNames;
  std::vector<const char *> outputNames;
  std::vector<std::string> inputNameStrings;
  std::vector<std::string> outputNameStrings;

  // Single-range ONNX inference over pre-built tensors (caller must
  // hold inferenceMutex). Returns (inEnd-inStart) * hopSize samples,
  // or empty on failure.
  std::vector<float> inferRangeLocked(const std::vector<float> &signal,
                                      const std::vector<std::vector<float>> &c,
                                      const std::array<std::vector<float>, 4> &dfs,
                                      size_t inStart, size_t inEnd);

  // (Re)creates onnxSession from the given model file; see
  // BigVGANVocoder::createSessionFor for the contract. Returns false
  // when the model's signature is not the expected 6-in/2-out.
  bool createSessionFor(const juce::File &file, bool wantDirectML,
                        GraphOptimizationLevel optLevel);

  // Runs the session ladder over modelFile and adopts the first
  // configuration whose probe inference succeeds.
  bool establishSession();

  // One short inference (16 frames of constant 200Hz) against the
  // current session, used to prove a ladder rung before it is adopted.
  bool probeCurrentSession(std::string &error);

  // infer()'s chunk loop against the current session. Sets lastError
  // on failure and returns empty; a shutdown or cancellation returns
  // empty without touching lastError.
  std::vector<float> inferWithCurrentSession(
      const std::vector<float> &cf0,
      const std::vector<std::vector<float>> &normalisedC,
      const std::shared_ptr<std::atomic<bool>> &cancelFlag);

  bool directMLProviderAppended = false;

  Ort::SessionOptions buildSessionOptions(bool wantDirectML,
                                          GraphOptimizationLevel optLevel);
#endif
};
