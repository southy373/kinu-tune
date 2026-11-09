#pragma once

#include "../JuceHeader.h"
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
 * BigVGAN v2 (44kHz / 128 band) neural vocoder wrapper, ONNX Runtime.
 *
 * In DirectML builds (Windows) the session runs on the GPU through the
 * DirectML execution provider, exactly like the PC-NSF-HiFiGAN Vocoder;
 * when DirectML cannot be appended or the session cannot be created with
 * it, the class quietly falls back to a CPU session. Other builds are
 * CPU-only.
 *
 * BigVGAN takes a mel spectrogram only - unlike the PC-NSF-HiFiGAN Vocoder
 * it has no F0 input, so pitch control has to happen before this class: the
 * caller renders the edited pitch into a scratch waveform (PSOLA), converts
 * that waveform to a BigVGAN mel (see BigVGANMel), and this class turns the
 * mel back into audio. It is far too slow for real-time use (roughly 10x
 * real time on CPU, less on a DirectML GPU), so it only ever runs on the
 * background re-render path, in chunks with context frames trimmed at the
 * joins.
 *
 * The model file (bigvgan_v2_44khz_128band.onnx, MIT, converted from the
 * official NVIDIA checkpoint by tools/convert_bigvgan.py) is loaded only
 * when present; the class mirrors Vocoder's session and worker management.
 */
class BigVGANVocoder
{
public:
  BigVGANVocoder();
  ~BigVGANVocoder();

  // Stop accepting work and let active inference exit at its next chunk.
  void requestShutdown();

  /**
   * Load the BigVGAN model from an ONNX file.
   * @param modelPath Path to bigvgan_v2_44khz_128band.onnx
   * @return true if successful
   */
  bool loadModel(const juce::File &modelPath);

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
   * Synthesize a waveform from a BigVGAN mel spectrogram.
   * @param mel Mel spectrogram [T, 128] in BigVGAN's log scale (BigVGANMel)
   * @return Synthesized waveform of T * hopSize samples, or empty on failure
   */
  std::vector<float> infer(const std::vector<std::vector<float>> &mel);

  /**
   * Asynchronous infer() with callback on the message thread.
   */
  void inferAsync(std::vector<std::vector<float>> mel,
                  std::function<void(std::vector<float>)> callback,
                  std::shared_ptr<std::atomic<bool>> cancelFlag = nullptr);

  /**
   * Asynchronous waveform-to-waveform resynthesis: the waveform (a scratch
   * render already carrying the edited pitch) is converted to a BigVGAN mel
   * and inferred on the worker thread. A waveform not at 44.1kHz is
   * resampled first; the result is always at 44.1kHz.
   */
  void inferFromWaveformAsync(
      std::vector<float> waveform, int sampleRate,
      std::function<void(std::vector<float>)> callback,
      std::shared_ptr<std::atomic<bool>> cancelFlag = nullptr);

  int getSampleRate() const { return sampleRate; }
  int getHopSize() const { return hopSize; }
  int getNumMels() const { return numMels; }

private:
  struct AsyncTask
  {
    enum class Kind
    {
      Mel,
      Waveform
    };
    Kind kind = Kind::Mel;
    std::vector<std::vector<float>> mel;
    std::vector<float> waveform;
    int sampleRate = 44100;
    std::function<void(std::vector<float>)> callback;
    std::shared_ptr<std::atomic<bool>> cancelFlag;
  };

  bool loaded = false;
  int sampleRate = 44100;
  int hopSize = 512;
  int numMels = 128;

  // Execution provider of the current session, set by loadModel() before
  // loaded becomes true (same plain-state style as Vocoder's device fields).
  bool usingGpuProvider = false;
  std::string activeProvider = "CPU";

  juce::File modelFile;
  std::string lastError;
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

  // Single-chunk ONNX inference (caller must hold inferenceMutex).
  // Returns (chunkFrames * hopSize) samples, or empty on failure.
  std::vector<float>
  inferChunkLocked(const std::vector<std::vector<float>> &mel);

  // (Re)creates onnxSession for modelFile and refreshes the cached
  // input/output names and provider state. cpuOnly builds the session
  // from createCpuFallbackSessionOptions() (no DirectML). Returns false
  // when the model's signature is not the expected 1-in/1-out; the
  // Ort::Session constructor's own failures propagate as Ort::Exception.
  bool createSession(bool cpuOnly);

  // infer()'s chunk loop against the current session. Sets lastError on
  // failure and returns empty; a shutdown mid-loop also returns empty
  // but leaves lastError alone (infer() checks isShuttingDown first).
  std::vector<float>
  inferWithCurrentSession(const std::vector<std::vector<float>> &mel);

  // True when createSessionOptions() appended the DirectML provider, so
  // loadModel() knows a session-creation failure is worth a CPU retry.
  bool directMLProviderAppended = false;

  Ort::SessionOptions createSessionOptions();
  Ort::SessionOptions createCpuFallbackSessionOptions();
#endif
};
