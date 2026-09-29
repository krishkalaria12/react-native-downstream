#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace downstream {

/// A fixed set of threads shared by every stream session and controlled
/// document, so rendering many Markdown views at once uses a bounded number of
/// cores instead of one thread each. Threads run below the priority of the UI
/// and JavaScript threads, so parsing and text preparation yield to input,
/// animation, and app logic when the CPU is busy.
class WorkerPool {
public:
  using Job = std::function<void()>;

  /// The process-wide pool. It is never destroyed, so jobs may outlive
  /// static destructors at exit.
  static WorkerPool &shared();

  /// Between one and four threads, leaving two cores for the UI and
  /// JavaScript threads.
  static size_t defaultThreadCount();

  explicit WorkerPool(size_t threadCount);
  ~WorkerPool();

  WorkerPool(const WorkerPool &) = delete;
  WorkerPool &operator=(const WorkerPool &) = delete;

  /// Queues a job. Jobs start in the order they are posted.
  void post(Job job);

  size_t threadCount() const { return threads_.size(); }

private:
  void run();

  std::mutex mutex_;
  std::condition_variable available_;
  std::deque<Job> jobs_;
  std::vector<std::thread> threads_;
  bool stopping_{false};
};

/// One piece of work that either a pool thread or a thread that needs its
/// result runs, whichever reaches it first. A caller that cannot continue
/// without the result never waits behind unrelated queued jobs.
class Task : public std::enable_shared_from_this<Task> {
public:
  explicit Task(std::function<void()> work) : work_(std::move(work)) {}

  /// Queues the task once. Later calls do nothing.
  void schedule(WorkerPool &pool = WorkerPool::shared());
  /// Runs the task on the calling thread if no thread has started it;
  /// otherwise blocks until it finishes.
  void runOrWait();
  bool done() const;
  /// Calls `callback` once the task finishes: on the finishing thread, or
  /// immediately on the calling thread when it has already finished.
  void onDone(std::function<void()> callback);

private:
  enum class State { Idle, Queued, Running, Done };

  bool claim();
  void execute();

  mutable std::mutex mutex_;
  std::condition_variable finished_;
  State state_{State::Idle};
  std::function<void()> work_;
  std::vector<std::function<void()>> callbacks_;
};

} // namespace downstream
