#include "WorkerPool.h"

#include <algorithm>
#include <utility>

#if defined(__APPLE__)
#include <pthread.h>
#include <sys/qos.h>
#elif defined(__ANDROID__) || defined(__linux__)
#include <pthread.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace downstream {
namespace {

constexpr size_t kMaxThreads = 4;
constexpr size_t kReservedCores = 2;

/// Below the UI and JavaScript threads, which run at user-interactive QoS on
/// iOS and at a higher Linux priority on Android.
void lowerCurrentThreadPriority() {
#if defined(__APPLE__)
  pthread_setname_np("downstream.worker");
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
#elif defined(__ANDROID__) || defined(__linux__)
  pthread_setname_np(pthread_self(), "ds-worker");
  // Android's THREAD_PRIORITY_BACKGROUND. Linux applies the nice value to the
  // thread named by the ID, not the process.
  setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), 10);
#endif
}

} // namespace

WorkerPool &WorkerPool::shared() {
  static auto *pool = new WorkerPool(defaultThreadCount());
  return *pool;
}

size_t WorkerPool::defaultThreadCount() {
  const size_t cores = std::thread::hardware_concurrency();
  return std::clamp<size_t>(cores > kReservedCores ? cores - kReservedCores : 1, 1, kMaxThreads);
}

WorkerPool::WorkerPool(size_t threadCount) {
  threadCount = std::max<size_t>(threadCount, 1);
  threads_.reserve(threadCount);
  for (size_t index = 0; index < threadCount; index++) {
    threads_.emplace_back([this] { run(); });
  }
}

WorkerPool::~WorkerPool() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
  }
  available_.notify_all();
  for (std::thread &thread : threads_) {
    thread.join();
  }
}

void WorkerPool::post(Job job) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    jobs_.push_back(std::move(job));
  }
  available_.notify_one();
}

void WorkerPool::run() {
  lowerCurrentThreadPriority();
  for (;;) {
    Job job;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      available_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
      if (jobs_.empty()) {
        return;
      }
      job = std::move(jobs_.front());
      jobs_.pop_front();
    }
    job();
  }
}

void Task::schedule(WorkerPool &pool) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != State::Idle) {
      return;
    }
    state_ = State::Queued;
  }
  pool.post([self = shared_from_this()] {
    if (self->claim()) {
      self->execute();
    }
  });
}

bool Task::claim() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (state_ != State::Idle && state_ != State::Queued) {
    return false;
  }
  state_ = State::Running;
  return true;
}

void Task::execute() {
  try {
    work_();
  } catch (...) {
    // The work reports its own failures through its result; a throw must not
    // leave waiters blocked.
  }
  std::vector<std::function<void()>> callbacks;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    state_ = State::Done;
    work_ = nullptr;
    callbacks.swap(callbacks_);
  }
  finished_.notify_all();
  for (auto &callback : callbacks) {
    callback();
  }
}

void Task::runOrWait() {
  if (claim()) {
    execute();
    return;
  }
  std::unique_lock<std::mutex> lock(mutex_);
  finished_.wait(lock, [this] { return state_ == State::Done; });
}

bool Task::done() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return state_ == State::Done;
}

void Task::onDone(std::function<void()> callback) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != State::Done) {
      callbacks_.push_back(std::move(callback));
      return;
    }
  }
  callback();
}

} // namespace downstream
