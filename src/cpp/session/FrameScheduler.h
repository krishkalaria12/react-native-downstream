#pragma once

#include "WorkerPool.h"

#include <cmath>
#include <functional>
#include <mutex>
#include <utility>
#include <vector>

namespace downstream {

struct FrameInfo {
  double intervalMs{1000.0 / 60.0};
  bool late{false};
};

/// One-shot native frame requests. Work posted during a frame is reserved for
/// the following frame. The host fallback uses asynchronous worker jobs.
class FrameScheduler {
public:
  using Job = std::function<void(const FrameInfo &)>;

  /// Install before creating sessions. An empty requester restores the host
  /// fallback. Requesters must arrange a future onFrame, never call it inline.
  static void install(std::function<void()> requestFrame) {
    std::lock_guard<std::mutex> lock(shared().mutex);
    shared().requestFrame = std::move(requestFrame);
  }

  static void post(Job job) {
    std::function<void()> request;
    {
      auto &state = shared();
      std::lock_guard<std::mutex> lock(state.mutex);
      state.jobs.push_back(std::move(job));
      if (state.requested || state.ticking) return;
      state.requested = true;
      request = state.requestFrame;
    }
    requestNext(std::move(request));
  }

  static void onFrame(double intervalMs = 1000.0 / 60.0, bool late = false) {
    std::vector<Job> jobs;
    {
      auto &state = shared();
      std::lock_guard<std::mutex> lock(state.mutex);
      if (state.ticking) return;
      state.requested = false;
      if (state.jobs.empty()) return;
      state.ticking = true;
      jobs.swap(state.jobs);
    }
    const FrameInfo info{std::isfinite(intervalMs) && intervalMs > 0 ? intervalMs : 1000.0 / 60.0, late};
    for (auto &job : jobs) job(info);
    std::function<void()> request;
    bool needsFrame = false;
    {
      auto &state = shared();
      std::lock_guard<std::mutex> lock(state.mutex);
      state.ticking = false;
      if (!state.jobs.empty() && !state.requested) {
        state.requested = true;
        needsFrame = true;
        request = state.requestFrame;
      }
    }
    if (needsFrame) requestNext(std::move(request));
  }

private:
  struct State {
    std::mutex mutex;
    std::vector<Job> jobs;
    std::function<void()> requestFrame;
    bool requested{false};
    bool ticking{false};
  };

  static State &shared() {
    static auto *state = new State;
    return *state;
  }

  static void requestNext(std::function<void()> request) {
    if (request) request();
    else WorkerPool::shared().post([] { onFrame(); });
  }
};

} // namespace downstream
