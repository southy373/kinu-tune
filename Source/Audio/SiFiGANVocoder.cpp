// Kinu Tune - Copyright (C) 2026 Keinoaza
// Part of Kinu Tune, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.

#include "SiFiGANVocoder.h"
#include "../Utils/AppLogger.h"
#include "../Utils/AudioResampler.h"
#include "../Utils/OnnxRuntime.h"
#include "../Utils/PlatformPaths.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>

namespace
{
  // Output frames generated per chunk, and the context inferred on
  // each side and trimmed. The context is wider than BigVGAN's: the
  // source network reads pitch-dependent offsets (pd_indexing) that
  // reach furthest at low F0, and the stage-2 harness measured the
  // chunked output against the single-shot one on real features -
  // 64 frames of context left an RMS difference of 3.9e-4, while 128
  // frames brought it to about 1e-6 (float32 noise). At 1024 output
  // frames the trimmed context costs about 25% extra inference.
  constexpr size_t kChunkFrames = 1024;
  constexpr size_t kContextFrames = 128;

  // The excitation noise is drawn from this fixed seed, so re-rendering
  // the same edit reproduces the same audio (training-time decode drew
  // fresh noise per run instead; a plugin render should be stable).
  constexpr std::uint64_t kExcitationSeed = 12345;

  // CPU thread budget for inference: about 70% of the logical cores,
  // never fewer than 2 - the same policy as BigVGANVocoder's, for the
  // same reason (the host DAW keeps the rest of the machine).
  int siFiGanCpuThreadCount()
  {
    return std::max(2, static_cast<int>(std::thread::hardware_concurrency() *
                                        0.7));
  }

  // Linear sample of a per-frame track at a fractional frame position,
  // clamped to the ends. Empty tracks read as 0.
  float sampleLinear(const std::vector<float> &track, double position)
  {
    if (track.empty())
      return 0.0f;
    if (position <= 0.0)
      return track.front();
    const auto last = static_cast<double>(track.size() - 1);
    if (position >= last)
      return track.back();
    const size_t index = static_cast<size_t>(position);
    const double frac = position - static_cast<double>(index);
    return static_cast<float>(track[index] * (1.0 - frac) +
                              track[index + 1] * frac);
  }

  double sampleLinear(const std::vector<double> &track, double position)
  {
    if (track.empty())
      return position;
    if (position <= 0.0)
      return track.front();
    const auto last = static_cast<double>(track.size() - 1);
    if (position >= last)
      return track.back();
    const size_t index = static_cast<size_t>(position);
    const double frac = position - static_cast<double>(index);
    return track[index] * (1.0 - frac) + track[index + 1] * frac;
  }

  // Nearest sample: for the edited F0, whose zero (unvoiced) pattern is
  // information the cf0 conversion relies on; linear interpolation
  // would invent low positive anchors at voiced/unvoiced boundaries.
  float sampleNearest(const std::vector<float> &track, double position)
  {
    if (track.empty())
      return 0.0f;
    const auto index = static_cast<size_t>(std::clamp(
        std::llround(position), 0LL,
        static_cast<long long>(track.size() - 1)));
    return track[index];
  }
} // namespace

SiFiGANVocoder::SiFiGANVocoder()
{
  auto logPath = PlatformPaths::getLogFile("sifigan_" +
                                           AppLogger::getSessionId() + ".txt");
  logFile = std::make_unique<std::ofstream>(
      logPath.getFullPathName().toStdString(), std::ios::app);

  if (logFile && logFile->is_open())
  {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    *logFile << "\n========== SiFi-GAN Session Started at "
             << std::ctime(&time) << " ==========\n";
    *logFile << "FlexaTune version: "
             << AppLogger::getAppVersion().toStdString() << "\n";
    logFile->flush();
  }

#ifdef HAVE_ONNXRUNTIME
  if (!OnnxRuntime::isAvailable())
  {
    log("ONNX Runtime unavailable: " +
        OnnxRuntime::getUnavailableReason().toStdString());
  }
  else try
  {
    onnxEnv =
        std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "PitchNet");
    onnxEnv->DisableTelemetryEvents();
    allocator = std::make_unique<Ort::AllocatorWithDefaultOptions>();
    log("ONNX Runtime initialized successfully");
  }
  catch (const Ort::Exception &e)
  {
    log("Failed to initialize ONNX Runtime: " + std::string(e.what()));
  }
#endif

  // Start async worker thread for inferFromWaveformAsync()
  asyncWorker = std::thread([this]()
                            {
    for (;;) {
      AsyncTask task;
      {
        std::unique_lock<std::mutex> lock(asyncMutex);
        asyncCondition.wait(lock, [this]() {
          return isShuttingDown.load() || !asyncQueue.empty();
        });

        if (isShuttingDown.load() && asyncQueue.empty())
          return;

        task = std::move(asyncQueue.front());
        asyncQueue.pop_front();
      }

      if (isShuttingDown.load()) {
        if (activeAsyncTasks.fetch_sub(1) == 1) {
          std::lock_guard<std::mutex> lock(asyncMutex);
          asyncCondition.notify_all();
        }
        continue;
      }

      // If canceled, still invoke callback (with empty result) so callers can
      // clear state and potentially schedule a rerun.
      if (task.cancelFlag && task.cancelFlag->load()) {
        if (activeAsyncTasks.fetch_sub(1) == 1) {
          std::lock_guard<std::mutex> lock(asyncMutex);
          asyncCondition.notify_all();
        }

        auto cb = std::move(task.callback);
        juce::MessageManager::callAsync([cb]() mutable {
          if (cb)
            cb({});
        });
        continue;
      }

      auto result = runTask(task);

      if (activeAsyncTasks.fetch_sub(1) == 1) {
        std::lock_guard<std::mutex> lock(asyncMutex);
        asyncCondition.notify_all();
      }

      if (isShuttingDown.load())
        continue;

      auto cb = std::move(task.callback);
      juce::MessageManager::callAsync(
          [cb = std::move(cb), result = std::move(result)]() mutable {
        if (cb)
          cb(std::move(result));
      });
    } });
}

void SiFiGANVocoder::requestShutdown()
{
  {
    std::lock_guard<std::mutex> lock(asyncMutex);
    isShuttingDown.store(true);
  }
  asyncCondition.notify_all();
}

SiFiGANVocoder::~SiFiGANVocoder()
{
  requestShutdown();

  if (asyncWorker.joinable())
    asyncWorker.join();

#ifdef HAVE_ONNXRUNTIME
  onnxSession.reset();
  onnxEnv.reset();
#endif
  if (logFile && logFile->is_open())
  {
    log("SiFi-GAN session ended");
    logFile->close();
  }
}

void SiFiGANVocoder::log(const std::string &message)
{
#if JUCE_DEBUG
  DBG("[SiFiGAN] " + juce::String(message));
#endif

  if (logFile && logFile->is_open())
  {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now.time_since_epoch()) %
              1000;

    std::tm tm_buf;
#ifdef _WIN32
    localtime_s(&tm_buf, &time);
#else
    localtime_r(&time, &tm_buf);
#endif

    *logFile << std::put_time(&tm_buf, "%H:%M:%S") << "." << std::setfill('0')
             << std::setw(3) << ms.count() << " | " << message << "\n";
    logFile->flush();
  }
}

bool SiFiGANVocoder::isOnnxRuntimeAvailable()
{
#ifdef HAVE_ONNXRUNTIME
  return OnnxRuntime::isAvailable();
#else
  return false;
#endif
}

bool SiFiGANVocoder::loadModel(const juce::File &modelPath,
                               const juce::File &statsPath)
{
#ifdef HAVE_ONNXRUNTIME
  if (!OnnxRuntime::isAvailable())
  {
    lastError = OnnxRuntime::getUnavailableReason().toStdString();
    log("SiFi-GAN: " + lastError);
    return false;
  }

  if (!onnxEnv)
  {
    log("ONNX Runtime not initialized");
    lastError = "ONNX Runtime not initialized";
    return false;
  }

  if (!modelPath.existsAsFile())
  {
    log("SiFi-GAN: Model file not found: " +
        modelPath.getFullPathName().toStdString());
    lastError = "Model file not found: " +
                modelPath.getFullPathName().toStdString();
    return false;
  }

  // The statistics are part of the model: without the scalers of the
  // training run that produced these weights, the features are wrong
  // by construction, so a missing or unusable sidecar is a load
  // failure, not something to default around.
  if (!statsPath.existsAsFile())
  {
    log("SiFi-GAN: Stats file not found: " +
        statsPath.getFullPathName().toStdString());
    lastError = "Stats file not found: " +
                statsPath.getFullPathName().toStdString();
    return false;
  }
  {
    sifigan::Stats parsed;
    std::string parseError;
    if (!sifigan::loadStatsFromJsonText(
            statsPath.loadFileAsString().toStdString(), parsed, &parseError))
    {
      log("SiFi-GAN: Stats file unusable: " + parseError);
      lastError = "Stats file unusable: " + parseError;
      return false;
    }
    stats = std::move(parsed);
  }

#ifdef _WIN32
  juce::String pathStr = modelPath.getFullPathName();
  if (pathStr.isEmpty() || pathStr.length() > 32767)
  {
    log("Invalid model path");
    lastError = "Invalid model path";
    return false;
  }
#endif

  log("Loading model from: " + modelPath.getFullPathName().toStdString());
  modelFile = modelPath;
  return establishSession();
#else
  (void)modelPath;
  (void)statsPath;
  log("SiFi-GAN: ONNX Runtime is not available; model inference is disabled");
  lastError = "ONNX Runtime is not available";
  loaded = false;
  return false;
#endif
}

#ifdef HAVE_ONNXRUNTIME
bool SiFiGANVocoder::createSessionFor(const juce::File &file,
                                      bool wantDirectML,
                                      GraphOptimizationLevel optLevel)
{
  Ort::SessionOptions sessionOptions =
      buildSessionOptions(wantDirectML, optLevel);
  const bool useGpu = wantDirectML && directMLProviderAppended;

#ifdef _WIN32
  const std::wstring modelPathW(file.getFullPathName().toWideCharPointer());
  onnxSession = std::make_unique<Ort::Session>(*onnxEnv, modelPathW.c_str(),
                                               sessionOptions);
#else
  const std::string modelPathStr = file.getFullPathName().toStdString();
  onnxSession = std::make_unique<Ort::Session>(*onnxEnv, modelPathStr.c_str(),
                                               sessionOptions);
#endif

  size_t numInputs = onnxSession->GetInputCount();
  inputNameStrings.clear();
  inputNames.clear();
  for (size_t i = 0; i < numInputs; ++i)
  {
    auto namePtr = onnxSession->GetInputNameAllocated(i, *allocator);
    inputNameStrings.push_back(namePtr.get());
  }
  for (auto &name : inputNameStrings)
    inputNames.push_back(name.c_str());

  size_t numOutputs = onnxSession->GetOutputCount();
  outputNameStrings.clear();
  outputNames.clear();
  for (size_t i = 0; i < numOutputs; ++i)
  {
    auto namePtr = onnxSession->GetOutputNameAllocated(i, *allocator);
    outputNameStrings.push_back(namePtr.get());
  }
  for (auto &name : outputNameStrings)
    outputNames.push_back(name.c_str());

  // signal / c / df0..df3 in, wav / src out - anything else is not the
  // SiFi-GAN generator this class drives.
  if (inputNames.size() != 6 || outputNames.size() != 2)
  {
    onnxSession.reset();
    usingGpuProvider = false;
    activeProvider = "CPU";
    return false;
  }

  usingGpuProvider = useGpu;
  activeProvider = usingGpuProvider ? "DirectML" : "CPU";
  activeModelFile = file;
  log("SiFi-GAN execution provider: " + activeProvider + " (model: " +
      file.getFileName().toStdString() + ")");
  return true;
}

bool SiFiGANVocoder::probeCurrentSession(std::string &error)
{
  // The smallest input that still runs every node of the graph: the
  // generator is fully convolutional, so 16 frames at a constant 200Hz
  // exercise the same nodes (including every ConvTranspose and Gather)
  // as a full chunk, at a fraction of the cost.
  constexpr size_t kProbeFrames = 16;
  const std::vector<float> cf0(kProbeFrames, 200.0f);
  const std::vector<std::vector<float>> c(
      kProbeFrames, std::vector<float>(sifigan::kFeatureDim, 0.0f));
  const auto signal = sifigan::buildExcitation(cf0, kExcitationSeed);
  const auto dfs = sifigan::buildDilatedFactors(cf0);

  std::lock_guard<std::mutex> lock(inferenceMutex);
  try
  {
    const auto out = inferRangeLocked(signal, c, dfs, 0, kProbeFrames);
    const size_t expected = kProbeFrames * sifigan::kHopSize;
    if (out.size() != expected)
    {
      error = "probe produced " + std::to_string(out.size()) +
              " samples, expected " + std::to_string(expected);
      return false;
    }
    return true;
  }
  catch (const Ort::Exception &e)
  {
    error = e.what();
    return false;
  }
}

bool SiFiGANVocoder::establishSession()
{
  struct Rung
  {
    std::string label;
    bool directML;
    GraphOptimizationLevel optLevel;
  };

  // One model file (there is no ConvTranspose-free variant of this
  // export), so the ladder is: DirectML with full, then basic graph
  // optimization, then CPU. Each rung is adopted only after the probe
  // inference succeeds on it.
  std::vector<Rung> ladder;
#ifdef USE_DIRECTML
  ladder.push_back({"DirectML + full graph optimization", true,
                    GraphOptimizationLevel::ORT_ENABLE_ALL});
  ladder.push_back({"DirectML + basic graph optimization", true,
                    GraphOptimizationLevel::ORT_ENABLE_BASIC});
#endif
  ladder.push_back({"CPU + full graph optimization", false,
                    GraphOptimizationLevel::ORT_ENABLE_ALL});

  log("SiFi-GAN session ladder: " + std::to_string(ladder.size()) +
      " rung(s)");
  const auto ladderStart = std::chrono::high_resolution_clock::now();
  std::string lastFailure;
  bool directMlAppendFailed = false;

  for (size_t i = 0; i < ladder.size(); ++i)
  {
    const auto &rung = ladder[i];
    if (rung.directML && directMlAppendFailed)
    {
      log("Session ladder rung " + std::to_string(i + 1) + " skipped (" +
          rung.label + "): the DirectML provider could not be appended");
      continue;
    }

    const auto rungStart = std::chrono::high_resolution_clock::now();
    const auto elapsedMs = [&rungStart]()
    {
      return std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::high_resolution_clock::now() - rungStart)
          .count();
    };
    log("Session ladder rung " + std::to_string(i + 1) + "/" +
        std::to_string(ladder.size()) + ": trying " + rung.label);

    bool signatureOk = false;
    try
    {
      signatureOk = createSessionFor(modelFile, rung.directML, rung.optLevel);
    }
    catch (const Ort::Exception &e)
    {
      lastFailure = std::string("session creation failed: ") + e.what();
      log("Session ladder rung " + std::to_string(i + 1) + " failed (" +
          rung.label + "): " + lastFailure + " [" +
          std::to_string(elapsedMs()) + "ms]");
      onnxSession.reset();
      if (rung.directML && !directMLProviderAppended)
        directMlAppendFailed = true;
      continue;
    }

    if (rung.directML && !directMLProviderAppended)
    {
      directMlAppendFailed = true;
      log("DirectML provider was not appended; the remaining DirectML "
          "rungs are skipped and this session is treated as the CPU rung");
    }

    if (!signatureOk)
    {
      lastFailure = "unexpected model signature (inputs=" +
                    std::to_string(inputNames.size()) +
                    ", outputs=" + std::to_string(outputNames.size()) + ")";
      log("Session ladder rung " + std::to_string(i + 1) + " failed (" +
          rung.label + "): " + lastFailure + " [" +
          std::to_string(elapsedMs()) + "ms]");
      continue;
    }

    std::string probeError;
    if (!probeCurrentSession(probeError))
    {
      lastFailure = "probe inference failed: " + probeError;
      log("Session ladder rung " + std::to_string(i + 1) + " failed (" +
          rung.label + "): " + lastFailure + " [" +
          std::to_string(elapsedMs()) + "ms]");
      onnxSession.reset();
      usingGpuProvider = false;
      activeProvider = "CPU";
      continue;
    }

    loaded = true;
    lastError.clear();
    const auto totalMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::high_resolution_clock::now() - ladderStart)
            .count();
    const std::string adoptedLabel =
        (rung.directML && !directMLProviderAppended)
            ? "CPU (DirectML provider could not be appended)"
            : rung.label;
    log("Session ladder: adopted rung " + std::to_string(i + 1) + " (" +
        adoptedLabel + "); probe passed in " +
        std::to_string(elapsedMs()) + "ms, ladder total " +
        std::to_string(totalMs) + "ms");
    log("SiFi-GAN: ONNX model loaded successfully");
    return true;
  }

  onnxSession.reset();
  usingGpuProvider = false;
  activeProvider = "CPU";
  loaded = false;
  lastError = "No SiFi-GAN session configuration could run inference" +
              (lastFailure.empty() ? std::string()
                                   : "; last failure: " + lastFailure);
  log("Session ladder: all rungs failed. " + lastError);
  return false;
}
#endif

std::vector<float> SiFiGANVocoder::infer(
    const std::vector<float> &cf0,
    const std::vector<std::vector<float>> &normalisedC,
    std::shared_ptr<std::atomic<bool>> cancelFlag)
{
  if (!loaded || cf0.empty() || normalisedC.empty())
    return {};

  std::lock_guard<std::mutex> lock(inferenceMutex);

  if (isShuttingDown.load())
    return {};

#ifdef HAVE_ONNXRUNTIME
  if (!onnxSession || inputNames.empty() || outputNames.empty())
  {
    lastError = "ONNX session not available";
    log(lastError);
    return {};
  }

  auto waveform = inferWithCurrentSession(cf0, normalisedC, cancelFlag);
  if (!waveform.empty())
  {
    lastError.clear();
    return waveform;
  }
  // A cancelled inference is not a DirectML failure (same reasoning as
  // BigVGANVocoder::infer): do not retry a superseded render on the CPU.
  if (!usingGpuProvider || isShuttingDown.load() ||
      (cancelFlag && cancelFlag->load()))
    return waveform;

  // Inference-level CPU fallback: recreate the session on the CPU and
  // retry the whole synthesis once; the session stays on the CPU.
  const std::string gpuError = lastError;
  log("DirectML inference failed (" + gpuError +
      "); recreating the session on CPU and retrying");
  const juce::File &fallbackFile =
      activeModelFile.existsAsFile() ? activeModelFile : modelFile;
  try
  {
    if (!createSessionFor(fallbackFile, false,
                          GraphOptimizationLevel::ORT_ENABLE_ALL))
    {
      lastError = "DirectML inference failed (" + gpuError +
                  "); CPU fallback session has an unexpected signature";
      log(lastError);
      return {};
    }
  }
  catch (const Ort::Exception &e)
  {
    lastError = "DirectML inference failed (" + gpuError +
                "); CPU fallback session could not be created: " +
                std::string(e.what());
    log(lastError);
    return {};
  }

  waveform = inferWithCurrentSession(cf0, normalisedC, cancelFlag);
  if (waveform.empty())
  {
    if (cancelFlag && cancelFlag->load())
      return waveform;
    lastError = "Inference failed on DirectML (" + gpuError +
                ") and on CPU (" + lastError + ")";
  }
  else
    lastError.clear();
  return waveform;
#else
  log("ONNX Runtime is not available");
  return {};
#endif
}

#ifdef HAVE_ONNXRUNTIME
std::vector<float> SiFiGANVocoder::inferWithCurrentSession(
    const std::vector<float> &cf0,
    const std::vector<std::vector<float>> &normalisedC,
    const std::shared_ptr<std::atomic<bool>> &cancelFlag)
{
  const size_t numFrames = normalisedC.size();
  const size_t totalSamples = numFrames * sifigan::kHopSize;
  const auto startTotal = std::chrono::high_resolution_clock::now();

  // The excitation and the dilated factors are built for the whole
  // span before chunking: the excitation phase is a cumulative sum, so
  // building it per chunk would restart the phase at every boundary.
  const auto signal = sifigan::buildExcitation(cf0, kExcitationSeed);
  const auto dfs = sifigan::buildDilatedFactors(cf0);

  try
  {
    std::vector<float> waveform;
    waveform.reserve(totalSamples);

    for (size_t chunkStart = 0; chunkStart < numFrames;
         chunkStart += kChunkFrames)
    {
      if (isShuttingDown.load())
        return {};

      if (cancelFlag && cancelFlag->load())
      {
        log("SiFi-GAN infer cancelled at frame " +
            std::to_string(chunkStart) + " of " +
            std::to_string(numFrames));
        return {};
      }

      const size_t chunkEnd =
          std::min(chunkStart + kChunkFrames, numFrames);
      const size_t inStart =
          chunkStart > kContextFrames ? chunkStart - kContextFrames : 0;
      const size_t inEnd = std::min(numFrames, chunkEnd + kContextFrames);

      auto chunkWav = inferRangeLocked(signal, normalisedC, dfs, inStart,
                                       inEnd);
      if (chunkWav.empty())
      {
        lastError = "Chunk inference failed at frame " +
                    std::to_string(chunkStart);
        log(lastError);
        return {};
      }

      const size_t skipSamples =
          (chunkStart - inStart) * static_cast<size_t>(sifigan::kHopSize);
      const size_t keepSamples =
          (chunkEnd - chunkStart) * static_cast<size_t>(sifigan::kHopSize);
      if (skipSamples + keepSamples > chunkWav.size())
      {
        lastError = "Chunk output shorter than expected at frame " +
                    std::to_string(chunkStart);
        log(lastError);
        return {};
      }
      waveform.insert(waveform.end(),
                      chunkWav.begin() +
                          static_cast<ptrdiff_t>(skipSamples),
                      chunkWav.begin() +
                          static_cast<ptrdiff_t>(skipSamples + keepSamples));
    }

    for (auto &s : waveform)
      s = std::clamp(s, -1.0f, 1.0f);

    const auto totalMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::high_resolution_clock::now() - startTotal)
            .count();
    log("SiFi-GAN infer [" + std::to_string(numFrames) +
        " frames] total=" + std::to_string(totalMs) + "ms");
    return waveform;
  }
  catch (const Ort::Exception &e)
  {
    lastError = std::string("ONNX inference failed: ") + e.what();
    log(lastError);
    return {};
  }
}

std::vector<float> SiFiGANVocoder::inferRangeLocked(
    const std::vector<float> &signal,
    const std::vector<std::vector<float>> &c,
    const std::array<std::vector<float>, 4> &dfs, size_t inStart,
    size_t inEnd)
{
  // Caller must hold inferenceMutex and have verified onnxSession is valid.
  const size_t numFrames = inEnd - inStart;
  if (numFrames == 0)
    return {};

  static const auto cpuMemoryInfo =
      Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

  // signal: (1, 1, frames * hop)
  const size_t signalSamples = numFrames * sifigan::kHopSize;
  std::vector<int64_t> signalShape{1, 1,
                                   static_cast<int64_t>(signalSamples)};
  auto signalTensor = Ort::Value::CreateTensor<float>(
      cpuMemoryInfo,
      const_cast<float *>(signal.data()) +
          inStart * static_cast<size_t>(sifigan::kHopSize),
      signalSamples, signalShape.data(), signalShape.size());

  // c: (1, 43, frames), channel-major from the [T][43] rows.
  std::vector<int64_t> cShape{1, sifigan::kFeatureDim,
                              static_cast<int64_t>(numFrames)};
  std::vector<float> cData(static_cast<size_t>(sifigan::kFeatureDim) *
                           numFrames);
  for (size_t frame = 0; frame < numFrames; ++frame)
  {
    const auto &sourceFrame = c[inStart + frame];
    const int dimCount =
        juce::jmin(sifigan::kFeatureDim,
                   static_cast<int>(sourceFrame.size()));
    size_t dstIndex = frame;
    int j = 0;
    for (; j < dimCount; ++j, dstIndex += numFrames)
      cData[dstIndex] = sourceFrame[static_cast<size_t>(j)];
    for (; j < sifigan::kFeatureDim; ++j, dstIndex += numFrames)
      cData[dstIndex] = 0.0f;
  }
  auto cTensor = Ort::Value::CreateTensor<float>(
      cpuMemoryInfo, cData.data(), cData.size(), cShape.data(),
      cShape.size());

  // df0..df3: (1, 1, frames * upsample), upsamples 5/20/60/120.
  constexpr int64_t upsamples[4] = {5, 20, 60, 120};
  std::vector<Ort::Value> inputs;
  inputs.push_back(std::move(signalTensor));
  inputs.push_back(std::move(cTensor));
  std::array<std::vector<int64_t>, 4> dfShapes;
  for (int k = 0; k < 4; ++k)
  {
    const size_t count = numFrames * static_cast<size_t>(upsamples[k]);
    dfShapes[static_cast<size_t>(k)] = {1, 1,
                                        static_cast<int64_t>(count)};
    inputs.push_back(Ort::Value::CreateTensor<float>(
        cpuMemoryInfo,
        const_cast<float *>(dfs[static_cast<size_t>(k)].data()) +
            inStart * static_cast<size_t>(upsamples[k]),
        count, dfShapes[static_cast<size_t>(k)].data(), 3));
  }

  static const Ort::RunOptions runOptions{nullptr};
  auto outputs = onnxSession->Run(runOptions, inputNames.data(),
                                  inputs.data(), inputs.size(),
                                  outputNames.data(), outputNames.size());
  if (outputs.empty() || !outputs.front().HasValue())
  {
    log("ONNX inference returned no output");
    return {};
  }

  auto typeInfo = outputs.front().GetTensorTypeAndShapeInfo();
  const size_t outputSize = typeInfo.GetElementCount();
  if (outputSize != signalSamples)
  {
    log("WARNING: Output length mismatch! Expected " +
        std::to_string(signalSamples) + " samples, got " +
        std::to_string(outputSize));
  }

  const float *outputData = outputs.front().GetTensorMutableData<float>();
  std::vector<float> waveform(outputSize);
  for (size_t i = 0; i < outputSize; ++i)
    waveform[i] = std::clamp(outputData[i], -1.0f, 1.0f);
  return waveform;
}

Ort::SessionOptions
SiFiGANVocoder::buildSessionOptions(bool wantDirectML,
                                    GraphOptimizationLevel optLevel)
{
  Ort::SessionOptions sessionOptions;
  sessionOptions.SetGraphOptimizationLevel(optLevel);
  sessionOptions.EnableMemPattern();
  sessionOptions.EnableCpuMemArena();

  directMLProviderAppended = false;

#ifdef USE_DIRECTML
  if (wantDirectML)
  {
    sessionOptions.SetIntraOpNumThreads(1);
    sessionOptions.SetInterOpNumThreads(1);
    try
    {
      const OrtApi &ortApi = Ort::GetApi();
      const OrtDmlApi *ortDmlApi = nullptr;
      Ort::ThrowOnError(ortApi.GetExecutionProviderApi(
          "DML", ORT_API_VERSION,
          reinterpret_cast<const void **>(&ortDmlApi)));

      sessionOptions.DisableMemPattern();
      sessionOptions.SetExecutionMode(ORT_SEQUENTIAL);

      Ort::ThrowOnError(ortDmlApi->SessionOptionsAppendExecutionProvider_DML(
          sessionOptions, /*device_id=*/0));
      directMLProviderAppended = true;
      log("DirectML execution provider added (device 0)");
    }
    catch (const Ort::Exception &e)
    {
      log("Failed to add DirectML provider: " + std::string(e.what()));
      log("Falling back to CPU");
    }

    if (!directMLProviderAppended)
    {
      sessionOptions.EnableMemPattern();
      sessionOptions.SetExecutionMode(ORT_PARALLEL);
      sessionOptions.SetIntraOpNumThreads(siFiGanCpuThreadCount());
    }
    return sessionOptions;
  }
#endif

  sessionOptions.SetIntraOpNumThreads(siFiGanCpuThreadCount());
  return sessionOptions;
}
#endif

void SiFiGANVocoder::enqueueTask(AsyncTask task)
{
  if (isShuttingDown.load())
  {
    log("enqueueTask: SiFiGANVocoder is shutting down, skipping request");
    return;
  }

  {
    std::lock_guard<std::mutex> lock(asyncMutex);
    if (isShuttingDown.load())
      return;
    activeAsyncTasks.fetch_add(1);
    asyncQueue.emplace_back(std::move(task));
    asyncCondition.notify_one();
  }
}

void SiFiGANVocoder::inferFromWaveformAsync(
    RenderRequest request,
    std::function<void(std::vector<float>)> callback,
    std::shared_ptr<std::atomic<bool>> cancelFlag)
{
  AsyncTask task;
  task.request = std::move(request);
  task.callback = std::move(callback);
  task.cancelFlag = std::move(cancelFlag);
  enqueueTask(std::move(task));
}

std::vector<float> SiFiGANVocoder::runTask(AsyncTask &task)
{
  return inferSync(task.request, task.cancelFlag);
}

std::vector<float> SiFiGANVocoder::inferSync(
    const RenderRequest &request,
    std::shared_ptr<std::atomic<bool>> cancelFlag)
{
  if (!loaded || request.sourceWaveform.empty() ||
      request.numOutputFrames <= 0 || request.analysisHop <= 0 ||
      request.sourceSampleRate <= 0)
    return {};

  // 1. The model works at 24kHz: resample the source span.
  std::vector<float> waveform24k;
  if (request.sourceSampleRate == sifigan::kSampleRate)
  {
    waveform24k = request.sourceWaveform;
  }
  else
  {
    juce::AudioBuffer<float> buffer(
        1, static_cast<int>(request.sourceWaveform.size()));
    buffer.copyFrom(0, 0, request.sourceWaveform.data(),
                    static_cast<int>(request.sourceWaveform.size()));
    auto resampled = AudioResampler::resample(
        buffer, static_cast<double>(request.sourceSampleRate),
        static_cast<double>(sifigan::kSampleRate));
    if (resampled.getNumSamples() <= 0)
      return {};
    waveform24k.assign(resampled.getReadPointer(0),
                       resampled.getReadPointer(0) +
                           resampled.getNumSamples());
  }

  if (cancelFlag && cancelFlag->load())
    return {};

  // Grid conversion factors. An analysis frame lasts
  // analysisHop/sourceSampleRate seconds; a SiFi frame lasts
  // hop/sampleRate seconds. Positions below are frame-start aligned on
  // both grids, the convention every sampling here shares.
  const double analysisFps =
      static_cast<double>(request.sourceSampleRate) / request.analysisHop;
  const double sifiFps =
      static_cast<double>(sifigan::kSampleRate) / sifigan::kHopSize;
  const double analysisToSifi = sifiFps / analysisFps;

  // 2. Source features: the analysed F0 on the SiFi grid, then the
  // training-identical WORLD analysis of the original audio.
  const int sourceFrames = static_cast<int>(std::floor(
      static_cast<double>(waveform24k.size()) / sifigan::kHopSize));
  std::vector<float> sourceF0Sifi(static_cast<size_t>(
      std::max(0, sourceFrames)));
  for (int t = 0; t < sourceFrames; ++t)
    sourceF0Sifi[static_cast<size_t>(t)] = sampleLinear(
        request.sourceF0, static_cast<double>(t) / analysisToSifi);

  const std::vector<double> waveform24kD(waveform24k.begin(),
                                         waveform24k.end());
  const auto analysis = sifigan::analyse(waveform24kD, sourceF0Sifi);
  if (analysis.numFrames == 0)
    return {};

  if (cancelFlag && cancelFlag->load())
    return {};

  // 3. Output features: for each output SiFi frame, follow the source
  // frame map (retiming) into the source features; the edited F0 is
  // sampled nearest so its voiced/unvoiced pattern survives, then made
  // continuous exactly as the training pipeline makes it.
  const int outputFrames = static_cast<int>(std::llround(
      static_cast<double>(request.numOutputFrames) * analysisToSifi));
  if (outputFrames <= 0)
    return {};

  sifigan::Analysis output;
  output.numFrames = outputFrames;
  output.mcep.assign(static_cast<size_t>(outputFrames),
                     std::vector<float>(sifigan::kMcepDim, 0.0f));
  output.bap.assign(static_cast<size_t>(outputFrames),
                    std::vector<float>(sifigan::kBapDim, 0.0f));
  std::vector<float> targetF0Sifi(static_cast<size_t>(outputFrames), 0.0f);
  for (int t = 0; t < outputFrames; ++t)
  {
    const double a = static_cast<double>(t) / analysisToSifi;
    const double sourceA =
        request.sourceFrameMap.empty()
            ? a
            : sampleLinear(request.sourceFrameMap, a);
    const double s = sourceA * analysisToSifi;
    // Linear interpolation of the source feature rows at s.
    const double pos = std::clamp(
        s, 0.0, static_cast<double>(analysis.numFrames - 1));
    const size_t srcIndex = static_cast<size_t>(pos);
    const double frac = pos - static_cast<double>(srcIndex);
    const size_t srcNext =
        std::min(srcIndex + 1,
                 static_cast<size_t>(analysis.numFrames - 1));
    const auto &mcepA = analysis.mcep[srcIndex];
    const auto &mcepB = analysis.mcep[srcNext];
    const auto &bapA = analysis.bap[srcIndex];
    const auto &bapB = analysis.bap[srcNext];
    auto &mcepFrame = output.mcep[static_cast<size_t>(t)];
    auto &bapFrame = output.bap[static_cast<size_t>(t)];
    for (int j = 0; j < sifigan::kMcepDim; ++j)
      mcepFrame[static_cast<size_t>(j)] = static_cast<float>(
          mcepA[static_cast<size_t>(j)] * (1.0 - frac) +
          mcepB[static_cast<size_t>(j)] * frac);
    for (int j = 0; j < sifigan::kBapDim; ++j)
      bapFrame[static_cast<size_t>(j)] = static_cast<float>(
          bapA[static_cast<size_t>(j)] * (1.0 - frac) +
          bapB[static_cast<size_t>(j)] * frac);
    targetF0Sifi[static_cast<size_t>(t)] =
        sampleNearest(request.targetF0, a);
  }
  output.cf0 = sifigan::makeContinuousF0(targetF0Sifi);

  const auto c = sifigan::normalise(output, stats);

  // 4. Generate at 24kHz, then resample back to the caller's rate and
  // trim to the render length (the synthesizer pads a short render
  // from the original, as it does for every engine).
  auto waveformOut24k = infer(output.cf0, c, cancelFlag);
  if (waveformOut24k.empty())
    return {};

  const size_t expectedSamples = static_cast<size_t>(request.numOutputFrames) *
                                 static_cast<size_t>(request.analysisHop);
  if (request.sourceSampleRate == sifigan::kSampleRate)
  {
    waveformOut24k.resize(
        std::min(waveformOut24k.size(), expectedSamples));
    return waveformOut24k;
  }

  juce::AudioBuffer<float> buffer24k(
      1, static_cast<int>(waveformOut24k.size()));
  buffer24k.copyFrom(0, 0, waveformOut24k.data(),
                     static_cast<int>(waveformOut24k.size()));
  auto back = AudioResampler::resample(
      buffer24k, static_cast<double>(sifigan::kSampleRate),
      static_cast<double>(request.sourceSampleRate));
  if (back.getNumSamples() <= 0)
    return {};
  std::vector<float> result(back.getReadPointer(0),
                            back.getReadPointer(0) + back.getNumSamples());
  result.resize(std::min(result.size(), expectedSamples));
  return result;
}
