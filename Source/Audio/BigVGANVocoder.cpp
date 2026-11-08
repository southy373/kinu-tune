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
  // crossfade artefacts entirely.
  constexpr size_t kChunkFrames = 200;
  constexpr size_t kContextFrames = 32;
} // namespace

BigVGANVocoder::BigVGANVocoder()
{
#if PITCHNET_LOGGING_ENABLED
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
    logFile->flush();
  }
#endif

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
#if PITCHNET_LOGGING_ENABLED
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
#else
  (void)message;
#endif
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
#ifdef HAVE_ONNXRUNTIME
  if (!onnxEnv)
  {
    log("ONNX Runtime not initialized");
    return false;
  }

  if (!modelPath.existsAsFile())
  {
    log("BigVGAN: Model file not found: " +
        modelPath.getFullPathName().toStdString());
    return false;
  }

  try
  {
    Ort::SessionOptions sessionOptions = createSessionOptions();

#ifdef _WIN32
    juce::String pathStr = modelPath.getFullPathName();
    if (pathStr.isEmpty())
    {
      log("Model path is empty");
      return false;
    }
    std::wstring modelPathW(pathStr.toWideCharPointer());
    if (modelPathW.empty() || modelPathW.length() > 32767)
    {
      log("Invalid model path length: " +
          std::to_string(modelPathW.length()));
      return false;
    }
    log("Loading model from: " + pathStr.toStdString());
    bool usedCpuFallback = false;
    try
    {
      onnxSession = std::make_unique<Ort::Session>(*onnxEnv, modelPathW.c_str(),
                                                   sessionOptions);
    }
    catch (const Ort::Exception &e)
    {
      if (!directMLProviderAppended)
        throw;

      log("Failed to create session with DirectML: " +
          std::string(e.what()));
      log("Retrying BigVGAN session with CPU");

      auto cpuOptions = createCpuFallbackSessionOptions();
      onnxSession = std::make_unique<Ort::Session>(*onnxEnv, modelPathW.c_str(),
                                                   cpuOptions);
      usedCpuFallback = true;
    }
#else
    std::string modelPathStr = modelPath.getFullPathName().toStdString();
    if (modelPathStr.empty())
    {
      log("Model path is empty");
      return false;
    }
    log("Loading model from: " + modelPathStr);
    bool usedCpuFallback = false;
    try
    {
      onnxSession = std::make_unique<Ort::Session>(
          *onnxEnv, modelPathStr.c_str(), sessionOptions);
    }
    catch (const Ort::Exception &e)
    {
      if (!directMLProviderAppended)
        throw;

      log("Failed to create session with DirectML: " +
          std::string(e.what()));
      log("Retrying BigVGAN session with CPU");

      auto cpuOptions = createCpuFallbackSessionOptions();
      onnxSession = std::make_unique<Ort::Session>(
          *onnxEnv, modelPathStr.c_str(), cpuOptions);
      usedCpuFallback = true;
    }
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
      log("BigVGAN: unexpected model signature (inputs=" +
          std::to_string(inputNames.size()) +
          ", outputs=" + std::to_string(outputNames.size()) + ")");
      onnxSession.reset();
      usingGpuProvider = false;
      activeProvider = "CPU";
      loaded = false;
      return false;
    }

    usingGpuProvider = directMLProviderAppended && !usedCpuFallback;
    activeProvider = usingGpuProvider ? "DirectML" : "CPU";
    log("BigVGAN execution provider: " + activeProvider);

    modelFile = modelPath;
    loaded = true;
    log("BigVGAN: ONNX model loaded successfully");
    return true;
  }
  catch (const Ort::Exception &e)
  {
    log("Failed to load ONNX model: " + std::string(e.what()));
    usingGpuProvider = false;
    activeProvider = "CPU";
    loaded = false;
    return false;
  }
#else
  (void)modelPath;
  log("BigVGAN: ONNX Runtime is not available; model inference is disabled");
  loaded = false;
  return false;
#endif
}

std::vector<float>
BigVGANVocoder::infer(const std::vector<std::vector<float>> &mel)
{
  if (!loaded || mel.empty())
    return {};

  std::lock_guard<std::mutex> lock(inferenceMutex);

  if (isShuttingDown.load())
    return {};

#ifdef HAVE_ONNXRUNTIME
  if (!onnxSession || inputNames.empty() || outputNames.empty())
  {
    log("ONNX session not available");
    return {};
  }

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
        log("Chunk inference failed at frame " +
            std::to_string(chunkStart));
        return {};
      }

      // Trim the context: keep only this chunk's own frames.
      const size_t skipSamples =
          (chunkStart - inStart) * static_cast<size_t>(hopSize);
      const size_t keepSamples =
          (chunkEnd - chunkStart) * static_cast<size_t>(hopSize);
      if (skipSamples + keepSamples > chunkWav.size())
      {
        log("Chunk output shorter than expected at frame " +
            std::to_string(chunkStart));
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
    log("ONNX inference failed: " + std::string(e.what()));
    return {};
  }
#else
  log("ONNX Runtime is not available");
  return {};
#endif
}

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

Ort::SessionOptions BigVGANVocoder::createSessionOptions()
{
  Ort::SessionOptions sessionOptions;
  sessionOptions.SetGraphOptimizationLevel(
      GraphOptimizationLevel::ORT_ENABLE_ALL);
  sessionOptions.EnableMemPattern();
  sessionOptions.EnableCpuMemArena();

  directMLProviderAppended = false;

#ifdef USE_DIRECTML
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
    const int numThreads =
        std::max(1u, std::thread::hardware_concurrency()) / 2;
    sessionOptions.SetIntraOpNumThreads(std::max(numThreads, 2));
  }
#else
  const int numThreads =
      std::max(1u, std::thread::hardware_concurrency()) / 2;
  sessionOptions.SetIntraOpNumThreads(std::max(numThreads, 2));
#endif
  return sessionOptions;
}

Ort::SessionOptions BigVGANVocoder::createCpuFallbackSessionOptions()
{
  Ort::SessionOptions sessionOptions;
  sessionOptions.SetGraphOptimizationLevel(
      GraphOptimizationLevel::ORT_ENABLE_ALL);
  sessionOptions.EnableMemPattern();
  sessionOptions.EnableCpuMemArena();

  const int numThreads =
      std::max(1u, std::thread::hardware_concurrency()) / 2;
  sessionOptions.SetIntraOpNumThreads(std::max(numThreads, 2));
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
    return infer(task.mel);

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

  return infer(mel);
}
