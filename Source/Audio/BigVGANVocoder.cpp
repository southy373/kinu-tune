#include "BigVGANVocoder.h"
#include "../Utils/AppLogger.h"
#include "../Utils/AudioResampler.h"
#include "../Utils/BigVGANMel.h"
#include "../Utils/OnnxRuntime.h"
#include "../Utils/PlatformPaths.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>

namespace
{
  // Output frames generated per chunk. BigVGAN is fully convolutional, so
  // chunking only needs enough context on both sides for the receptive
  // field; the context frames are inferred and then trimmed, which avoids
  // crossfade artefacts entirely. At 512 output frames the trimmed
  // context (32 frames a side) costs about 12% extra inference, against
  // about 27% at the previous 200.
  constexpr size_t kChunkFrames = 512;
  constexpr size_t kContextFrames = 32;

  // CPU thread budget for BigVGAN inference: about 70% of the logical
  // cores (12 threads -> 8 on a Core i5-10400F), never fewer than 2.
  // Rendering runs on a background worker while the host DAW keeps
  // playing back and handling its UI, so a share of the machine is
  // deliberately left alone instead of taking every core.
  int bigVganCpuThreadCount()
  {
    return std::max(2, static_cast<int>(std::thread::hardware_concurrency() *
                                        0.7));
  }
} // namespace

BigVGANVocoder::BigVGANVocoder()
{
  // Batch 3: this log is the only record of why an AI render failed on a
  // user's machine, so it is written in every build. It used to sit
  // behind PITCHNET_LOGGING_ENABLED, which Release builds turn off -
  // exactly the builds users run - leaving failures undiagnosable.
  auto logPath = PlatformPaths::getLogFile("bigvgan_" +
                                           AppLogger::getSessionId() + ".txt");
  logFile = std::make_unique<std::ofstream>(
      logPath.getFullPathName().toStdString(), std::ios::app);

  if (logFile && logFile->is_open())
  {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    *logFile << "\n========== BigVGAN Session Started at "
             << std::ctime(&time) << " ==========\n";
    // Batch 4 fix (0.4.2): name the build in the log header, so a
    // trial log shows which version produced it.
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

  // Start async worker thread for inferAsync()
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

void BigVGANVocoder::requestShutdown()
{
  {
    std::lock_guard<std::mutex> lock(asyncMutex);
    isShuttingDown.store(true);
  }
  asyncCondition.notify_all();
}

BigVGANVocoder::~BigVGANVocoder()
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
    log("BigVGAN session ended");
    logFile->close();
  }
}

void BigVGANVocoder::log(const std::string &message)
{
#if JUCE_DEBUG
  DBG("[BigVGAN] " + juce::String(message));
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

bool BigVGANVocoder::isOnnxRuntimeAvailable()
{
#ifdef HAVE_ONNXRUNTIME
  return OnnxRuntime::isAvailable();
#else
  return false;
#endif
}

bool BigVGANVocoder::loadModel(const juce::File &modelPath)
{
  // The ConvTranspose-free variant ships next to the main model; a
  // hand-placed copy is found the same way. Callers that resolve model
  // paths through PlatformPaths pass it explicitly instead (the
  // two-arg overload).
  return loadModel(
      modelPath,
      modelPath.getSiblingFile("bigvgan_v2_44khz_128band_noct.onnx"));
}

bool BigVGANVocoder::loadModel(const juce::File &modelPath,
                               const juce::File &noctModelPath)
{
#ifdef HAVE_ONNXRUNTIME
  if (!onnxEnv)
  {
    log("ONNX Runtime not initialized");
    lastError = "ONNX Runtime not initialized";
    return false;
  }

  if (!modelPath.existsAsFile())
  {
    log("BigVGAN: Model file not found: " +
        modelPath.getFullPathName().toStdString());
    lastError = "Model file not found: " +
                modelPath.getFullPathName().toStdString();
    return false;
  }

#ifdef _WIN32
  juce::String pathStr = modelPath.getFullPathName();
  if (pathStr.isEmpty())
  {
    log("Model path is empty");
    lastError = "Model path is empty";
    return false;
  }
  std::wstring modelPathW(pathStr.toWideCharPointer());
  if (modelPathW.empty() || modelPathW.length() > 32767)
  {
    log("Invalid model path length: " +
        std::to_string(modelPathW.length()));
    lastError = "Invalid model path";
    return false;
  }
#endif

  log("Loading model from: " + modelPath.getFullPathName().toStdString());
  modelFile = modelPath;
  noctModelFile = (noctModelPath.existsAsFile() &&
                   noctModelPath != modelPath)
                      ? noctModelPath
                      : juce::File{};
  log(noctModelFile.existsAsFile()
          ? "Noct variant present: " +
                noctModelFile.getFullPathName().toStdString()
          : "Noct variant not present; the ladder runs without it");

  return establishSession();
#else
  (void)modelPath;
  (void)noctModelPath;
  log("BigVGAN: ONNX Runtime is not available; model inference is disabled");
  lastError = "ONNX Runtime is not available";
  loaded = false;
  return false;
#endif
}

#ifdef HAVE_ONNXRUNTIME
bool BigVGANVocoder::createSessionFor(const juce::File &file,
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

  if (inputNames.size() != 1 || outputNames.size() != 1)
  {
    onnxSession.reset();
    usingGpuProvider = false;
    activeProvider = "CPU";
    return false;
  }

  usingGpuProvider = useGpu;
  activeProvider = usingGpuProvider ? "DirectML" : "CPU";
  activeModelFile = file;
  log("BigVGAN execution provider: " + activeProvider + " (model: " +
      file.getFileName().toStdString() + ")");
  return true;
}

bool BigVGANVocoder::probeCurrentSession(std::string &error)
{
  // The smallest mel that still runs every node of the graph: BigVGAN is
  // fully convolutional, so 16 frames exercise the same nodes (including,
  // in the main model, every ConvTranspose) as a full chunk, at a
  // fraction of the cost.
  constexpr size_t kProbeFrames = 16;
  std::vector<std::vector<float>> mel(
      kProbeFrames, std::vector<float>(static_cast<size_t>(numMels)));
  for (size_t frame = 0; frame < kProbeFrames; ++frame)
    for (int m = 0; m < numMels; ++m)
      mel[frame][static_cast<size_t>(m)] =
          -6.0f + 0.05f * static_cast<float>((frame * 31 + m * 7) % 97);

  // inferChunkLocked's contract is that the caller holds
  // inferenceMutex. The ladder runs before the vocoder is marked
  // loaded, so no render could race this probe anyway, but the lock
  // is taken so the contract holds formally as well as in practice
  // (batch 4 review carry-over).
  std::lock_guard<std::mutex> lock(inferenceMutex);
  try
  {
    const auto out = inferChunkLocked(mel);
    const size_t expected = kProbeFrames * static_cast<size_t>(hopSize);
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

bool BigVGANVocoder::establishSession()
{
  struct Rung
  {
    std::string label;
    juce::File file;
    bool directML;
    GraphOptimizationLevel optLevel;
  };

  // Batch 4 ladder order: the ConvTranspose-free noct variant goes
  // first on DirectML - it is the only rung that avoids the operator
  // the provider rejected in the field (see the class comment). The
  // main model keeps its batch 3.1 DirectML chances behind it, and
  // the CPU rung is unchanged. Without the noct file the ladder is
  // exactly batch 3.1's.
  std::vector<Rung> ladder;
#ifdef USE_DIRECTML
  if (noctModelFile.existsAsFile())
  {
    ladder.push_back({"DirectML + noct model (ConvTranspose-free) + full "
                      "graph optimization",
                      noctModelFile, true,
                      GraphOptimizationLevel::ORT_ENABLE_ALL});
    ladder.push_back({"DirectML + noct model (ConvTranspose-free) + basic "
                      "graph optimization",
                      noctModelFile, true,
                      GraphOptimizationLevel::ORT_ENABLE_BASIC});
  }
  ladder.push_back({"DirectML + main model (opset 20) + full graph "
                    "optimization",
                    modelFile, true, GraphOptimizationLevel::ORT_ENABLE_ALL});
  ladder.push_back({"DirectML + main model (opset 20) + basic graph "
                    "optimization",
                    modelFile, true,
                    GraphOptimizationLevel::ORT_ENABLE_BASIC});
#endif
  ladder.push_back({"CPU + main model (opset 20) + full graph optimization",
                    modelFile, false,
                    GraphOptimizationLevel::ORT_ENABLE_ALL});

  log("BigVGAN session ladder: " + std::to_string(ladder.size()) +
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
      signatureOk =
          createSessionFor(rung.file, rung.directML, rung.optLevel);
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
      // The provider could not be appended at all, so this "DirectML"
      // session is a CPU session in disguise and the remaining DirectML
      // rungs would test the same thing again. The CPU rung below is
      // the same configuration as rung 1, so stop the ladder here and
      // let it run.
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
    // When the DirectML provider could not be appended, this rung's
    // session is a CPU session despite its DirectML label (see the
    // append-failure branch above); the adopted line must say so, or
    // the log claims a GPU configuration that never ran (batch 4
    // review carry-over).
    const std::string adoptedLabel =
        (rung.directML && !directMLProviderAppended)
            ? "CPU + " + rung.file.getFileName().toStdString() +
                  " (DirectML provider could not be appended)"
            : rung.label;
    log("Session ladder: adopted rung " + std::to_string(i + 1) + " (" +
        adoptedLabel + "); probe passed in " +
        std::to_string(elapsedMs()) + "ms, ladder total " +
        std::to_string(totalMs) + "ms");
    log("BigVGAN: ONNX model loaded successfully");
    return true;
  }

  onnxSession.reset();
  usingGpuProvider = false;
  activeProvider = "CPU";
  loaded = false;
  lastError = "No BigVGAN session configuration could run inference" +
              (lastFailure.empty() ? std::string()
                                   : "; last failure: " + lastFailure);
  log("Session ladder: all rungs failed. " + lastError);
  return false;
}
#endif

std::vector<float> BigVGANVocoder::infer(
    const std::vector<std::vector<float>> &mel,
    std::shared_ptr<std::atomic<bool>> cancelFlag)
{
  if (!loaded || mel.empty())
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

  auto waveform = inferWithCurrentSession(mel, cancelFlag);
  if (!waveform.empty())
  {
    // A successful render clears any earlier failure: lastError feeds
    // the editor's render status, and a stale reason would otherwise be
    // paired with a later, unrelated failure (batch 3 review, minor 1).
    lastError.clear();
    return waveform;
  }
  // Batch 5: a cancelled inference is not a DirectML failure. Without
  // this check the empty result would fall into the CPU retry below and
  // the render the user just superseded would start over from the
  // first chunk on the CPU - the exact stall cancellation exists to
  // remove. The flag is re-checked here, immediately before the retry,
  // so a cancel that lands while the GPU attempt is failing also stops
  // the retry.
  if (!usingGpuProvider || isShuttingDown.load() ||
      (cancelFlag && cancelFlag->load()))
    return waveform;

  // Batch 3: inference-level CPU fallback. A DirectML session can be
  // created successfully and still fail on the first real chunk - the
  // device-0 adapter on a multi-GPU machine is not necessarily one that
  // can run this graph, and no CI runner exercises a real GPU, so
  // creation-time fallback alone left the AI engine dead on such
  // machines. The batch 3.1 session ladder probes for this at load
  // time, but a real chunk is much longer than the probe and can still
  // fail where the probe passed, so the retry stays: recreate the
  // session on the CPU (same model file the live session was built
  // from) and retry the whole synthesis once; the session stays on the
  // CPU afterwards.
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

  // The retry honours the same cancel flag: a cancel that lands while
  // the fallback session is being created stops the retry at its first
  // chunk boundary instead of running the whole synthesis.
  waveform = inferWithCurrentSession(mel, cancelFlag);
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
std::vector<float> BigVGANVocoder::inferWithCurrentSession(
    const std::vector<std::vector<float>> &mel,
    const std::shared_ptr<std::atomic<bool>> &cancelFlag)
{
  const size_t numFrames = mel.size();
  const size_t totalSamples = numFrames * static_cast<size_t>(hopSize);
  const auto startTotal = std::chrono::high_resolution_clock::now();

  try
  {
    std::vector<float> waveform;
    waveform.reserve(totalSamples);

    for (size_t chunkStart = 0; chunkStart < numFrames;
         chunkStart += kChunkFrames)
    {
      if (isShuttingDown.load())
        return {};

      // Batch 5: honour the job's cancel flag at chunk boundaries.
      // Until now the loop only watched the shutdown flag, so a
      // superseded AI render ran every remaining chunk to completion
      // while the newer edit (or Undo, or engine switch) waited behind
      // it. A cancelled chunk's Run() has already finished normally,
      // so the session stays usable; the partial waveform is dropped
      // by returning empty, exactly like a failure, but lastError is
      // NOT set - infer() distinguishes the two through the flag and
      // skips its CPU retry for a cancel.
      if (cancelFlag && cancelFlag->load())
      {
        log("BigVGAN infer cancelled at frame " +
            std::to_string(chunkStart) + " of " +
            std::to_string(numFrames));
        return {};
      }

      const size_t chunkEnd = std::min(chunkStart + kChunkFrames, numFrames);
      const size_t inStart =
          chunkStart > kContextFrames ? chunkStart - kContextFrames : 0;
      const size_t inEnd = std::min(numFrames, chunkEnd + kContextFrames);

      std::vector<std::vector<float>> chunkMel(
          mel.begin() + static_cast<ptrdiff_t>(inStart),
          mel.begin() + static_cast<ptrdiff_t>(inEnd));
      auto chunkWav = inferChunkLocked(chunkMel);
      if (chunkWav.empty())
      {
        lastError = "Chunk inference failed at frame " +
                    std::to_string(chunkStart);
        log(lastError);
        return {};
      }

      // Trim the context: keep only this chunk's own frames.
      const size_t skipSamples =
          (chunkStart - inStart) * static_cast<size_t>(hopSize);
      const size_t keepSamples =
          (chunkEnd - chunkStart) * static_cast<size_t>(hopSize);
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
    log("BigVGAN infer [" + std::to_string(numFrames) +
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
#endif

#ifdef HAVE_ONNXRUNTIME
std::vector<float> BigVGANVocoder::inferChunkLocked(
    const std::vector<std::vector<float>> &mel)
{
  // Caller must hold inferenceMutex and have verified onnxSession is valid.
  const size_t numFrames = mel.size();
  if (numFrames == 0)
    return {};

  // Prepare mel input: [batch=1, num_mels, frames]
  std::vector<int64_t> melShape{1, static_cast<int64_t>(numMels),
                                static_cast<int64_t>(numFrames)};
  std::vector<float> melData(static_cast<size_t>(numMels) * numFrames);
  for (size_t frame = 0; frame < numFrames; ++frame)
  {
    const auto &sourceFrame = mel[frame];
    const int melCount =
        juce::jmin(numMels, static_cast<int>(sourceFrame.size()));
    size_t dstIndex = frame;
    int m = 0;
    for (; m < melCount; ++m, dstIndex += numFrames)
      melData[dstIndex] = sourceFrame[static_cast<size_t>(m)];
    for (; m < numMels; ++m, dstIndex += numFrames)
      melData[dstIndex] = std::log(1e-5f);
  }

  static const auto cpuMemoryInfo =
      Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

  auto inputTensor = Ort::Value::CreateTensor<float>(
      cpuMemoryInfo, melData.data(), melData.size(), melShape.data(),
      melShape.size());

  static const Ort::RunOptions runOptions{nullptr};
  auto outputs = onnxSession->Run(runOptions, inputNames.data(),
                                  &inputTensor, 1, outputNames.data(), 1);
  if (outputs.empty() || !outputs.front().HasValue())
  {
    log("ONNX inference returned no output");
    return {};
  }

  auto typeInfo = outputs.front().GetTensorTypeAndShapeInfo();
  const size_t outputSize = typeInfo.GetElementCount();
  const size_t expectedSamples = numFrames * static_cast<size_t>(hopSize);
  if (outputSize != expectedSamples)
  {
    log("WARNING: Output length mismatch! Expected " +
        std::to_string(expectedSamples) + " samples, got " +
        std::to_string(outputSize));
  }

  float *outputData = outputs.front().GetTensorMutableData<float>();
  std::vector<float> waveform(outputSize);
  for (size_t i = 0; i < outputSize; ++i)
    waveform[i] = std::clamp(outputData[i], -1.0f, 1.0f);
  return waveform;
}

Ort::SessionOptions
BigVGANVocoder::buildSessionOptions(bool wantDirectML,
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
    // DirectML build: run BigVGAN on the GPU, in the same way as the NSF
    // vocoder (Vocoder::createSessionOptions). GPU-backed providers run
    // best with minimal ORT CPU thread pools.
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
      // The provider could not be appended, so this session runs on the
      // CPU: undo the settings DirectML needed changed.
      sessionOptions.EnableMemPattern();
      sessionOptions.SetExecutionMode(ORT_PARALLEL);
      sessionOptions.SetIntraOpNumThreads(bigVganCpuThreadCount());
    }
    return sessionOptions;
  }
#endif

  // CPU session (the ladder's last rung, the inference-time fallback,
  // and every non-DirectML build): the ~70% thread cap is unchanged.
  sessionOptions.SetIntraOpNumThreads(bigVganCpuThreadCount());
  return sessionOptions;
}
#endif

void BigVGANVocoder::enqueueTask(AsyncTask task)
{
  if (isShuttingDown.load())
  {
    log("enqueueTask: BigVGANVocoder is shutting down, skipping request");
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

void BigVGANVocoder::inferAsync(
    std::vector<std::vector<float>> mel,
    std::function<void(std::vector<float>)> callback,
    std::shared_ptr<std::atomic<bool>> cancelFlag)
{
  AsyncTask task;
  task.kind = AsyncTask::Kind::Mel;
  task.mel = std::move(mel);
  task.callback = std::move(callback);
  task.cancelFlag = std::move(cancelFlag);
  enqueueTask(std::move(task));
}

void BigVGANVocoder::inferFromWaveformAsync(
    std::vector<float> waveform, int sampleRate,
    std::function<void(std::vector<float>)> callback,
    std::shared_ptr<std::atomic<bool>> cancelFlag)
{
  AsyncTask task;
  task.kind = AsyncTask::Kind::Waveform;
  task.waveform = std::move(waveform);
  task.sampleRate = sampleRate;
  task.callback = std::move(callback);
  task.cancelFlag = std::move(cancelFlag);
  enqueueTask(std::move(task));
}

std::vector<float> BigVGANVocoder::runTask(AsyncTask &task)
{
  if (task.kind == AsyncTask::Kind::Mel)
    return infer(task.mel, task.cancelFlag);

  // Waveform task: convert the scratch render to a BigVGAN mel, then infer.
  if (task.waveform.empty())
    return {};

  std::vector<float> waveform44k;
  if (task.sampleRate == BigVGANMel::kSampleRate)
  {
    waveform44k = std::move(task.waveform);
  }
  else
  {
    // BigVGAN is a 44.1kHz model. Other session rates are resampled for the
    // mel computation; the caller pads the result back to the render length
    // from the original audio, as it does for any short render.
    juce::AudioBuffer<float> buffer(1,
                                    static_cast<int>(task.waveform.size()));
    buffer.copyFrom(0, 0, task.waveform.data(),
                    static_cast<int>(task.waveform.size()));
    auto resampled = AudioResampler::resample(
        buffer, static_cast<double>(task.sampleRate),
        static_cast<double>(BigVGANMel::kSampleRate));
    if (resampled.getNumSamples() <= 0)
      return {};
    waveform44k.assign(resampled.getReadPointer(0),
                       resampled.getReadPointer(0) +
                           resampled.getNumSamples());
  }

  if (task.cancelFlag && task.cancelFlag->load())
    return {};

  BigVGANMel melComputer;
  auto mel = melComputer.compute(waveform44k.data(),
                                 static_cast<int>(waveform44k.size()));
  if (mel.empty())
    return {};

  if (task.cancelFlag && task.cancelFlag->load())
    return {};

  return infer(mel, task.cancelFlag);
}
