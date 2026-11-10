// Kinu Tune - Copyright (C) 2026 Keinoaza
// Part of Kinu Tune, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.
#pragma once

#include "DspSynthesisRequest.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

/**
 * The render queue PsolaSynthesizer has always run - one shared worker
 * thread per engine, tasks in post order, the callback invoked on the
 * worker thread with the rendered samples (empty on cancellation) -
 * factored out in batch 4 so the Rubber Band and Signalsmith
 * pitch-shift engines share the exact same arrangement instead of
 * growing a second and third copy of it. Each engine keeps its worker
 * in a function-local static, as PSOLA's RenderWorker is kept, so all
 * renders of one engine stay serialised on a single thread and the
 * plug-in's CPU budget is unchanged; only the selected engine's worker
 * ever receives tasks.
 */
class AsyncDspRenderWorker
{
public:
  using RenderFn = std::function<std::vector<float>(
      const DspSynthesisRequest &,
      const std::shared_ptr<std::atomic<bool>> &)>;

  explicit AsyncDspRenderWorker(RenderFn renderFn)
      : render(std::move(renderFn))
  {
    thread = std::thread([this] { run(); });
  }

  ~AsyncDspRenderWorker()
  {
    {
      std::lock_guard<std::mutex> lock(mutex);
      shuttingDown = true;
    }
    condition.notify_all();
    if (thread.joinable())
      thread.join();
  }

  AsyncDspRenderWorker(const AsyncDspRenderWorker &) = delete;
  AsyncDspRenderWorker &operator=(const AsyncDspRenderWorker &) = delete;

  void post(DspSynthesisRequest request,
            std::function<void(std::vector<float>)> callback,
            std::shared_ptr<std::atomic<bool>> cancelFlag)
  {
    {
      std::lock_guard<std::mutex> lock(mutex);
      queue.push_back(Task{std::move(request), std::move(callback),
                           std::move(cancelFlag)});
    }
    condition.notify_one();
  }

private:
  struct Task
  {
    DspSynthesisRequest request;
    std::function<void(std::vector<float>)> callback;
    std::shared_ptr<std::atomic<bool>> cancelFlag;
  };

  void run()
  {
    for (;;)
    {
      Task task;
      {
        std::unique_lock<std::mutex> lock(mutex);
        condition.wait(
            lock, [this] { return shuttingDown || !queue.empty(); });
        if (shuttingDown)
          return;
        task = std::move(queue.front());
        queue.pop_front();
      }

      std::vector<float> result;
      if (task.cancelFlag == nullptr || !task.cancelFlag->load())
        result = render(task.request, task.cancelFlag);

      if (task.callback)
        task.callback(std::move(result));
    }
  }

  RenderFn render;
  std::mutex mutex;
  std::condition_variable condition;
  std::deque<Task> queue;
  bool shuttingDown = false;
  std::thread thread;
};
