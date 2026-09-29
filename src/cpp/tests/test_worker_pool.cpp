#include "PreparedDocument.h"
#include "RenderPreparer.h"
#include "SessionRegistry.h"
#include "WorkerPool.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace downstream;
using namespace std::chrono_literals;

namespace {

bool fail(const char *message) {
  std::cerr << message << "\n";
  return false;
}

// Every pool thread runs a job at the same time.
bool poolRunsJobsInParallel() {
  std::mutex mutex;
  std::condition_variable changed;
  size_t arrived = 0;
  bool released = false;
  // Declared last, so its threads are joined before the state they use goes.
  WorkerPool pool(3);
  for (int i = 0; i < 3; i++) {
    pool.post([&] {
      std::unique_lock<std::mutex> lock(mutex);
      arrived++;
      changed.notify_all();
      changed.wait(lock, [&] { return released; });
    });
  }
  std::unique_lock<std::mutex> lock(mutex);
  const bool parallel = changed.wait_for(lock, 3s, [&] { return arrived == 3; });
  released = true;
  changed.notify_all();
  return parallel || fail("Pool did not run its jobs in parallel");
}

// A thread that needs a result runs a queued task itself rather than wait
// behind unrelated work.
bool waiterRunsQueuedTask() {
  std::mutex mutex;
  std::condition_variable changed;
  bool blocked = false;
  bool released = false;
  WorkerPool pool(1);
  pool.post([&] {
    std::unique_lock<std::mutex> lock(mutex);
    blocked = true;
    changed.notify_all();
    changed.wait(lock, [&] { return released; });
  });
  {
    std::unique_lock<std::mutex> lock(mutex);
    changed.wait(lock, [&] { return blocked; });
  }
  std::thread::id ranOn;
  auto task = std::make_shared<Task>([&] { ranOn = std::this_thread::get_id(); });
  task->schedule(pool);
  task->runOrWait();
  const bool inline_ = task->done() && ranOn == std::this_thread::get_id();
  bool calledNow = false;
  task->onDone([&] { calledNow = true; });
  {
    std::lock_guard<std::mutex> lock(mutex);
    released = true;
  }
  changed.notify_all();
  return (inline_ && calledNow) || fail("Waiting thread did not run the queued task");
}

std::mutex preparedMutex;
std::vector<std::pair<uint64_t, LayoutHint>> preparedCalls;

void recordPrepare(const DocumentHandle &document, const std::vector<LayoutHint> &hints) {
  std::lock_guard<std::mutex> lock(preparedMutex);
  for (const LayoutHint &hint : hints) {
    preparedCalls.emplace_back(document->id, hint);
  }
}

bool preparedDocumentsAreSharedAndPrepared() {
  RenderPreparer::install(&recordPrepare);
  RenderPreparer::noteLayout({1, 320});
  RenderPreparer::noteLayout({1, 280});
  RenderPreparer::noteLayout({1, 320});
  const auto recent = RenderPreparer::recentLayouts();
  if (recent.size() != 2 || !(recent[0] == LayoutHint{1, 320}) || !(recent[1] == LayoutHint{1, 280})) {
    return fail("Recent layouts are not newest first and distinct");
  }

  const auto first = PreparedDocument::request("# Title\n\nSome **bold** text.");
  const auto second = PreparedDocument::request("# Title\n\nSome **bold** text.");
  first->runOrWait();
  if (first != second || !first->done() || first->result().status != ParseStatus::Ok ||
      first->result().document->blocks.size() != 2) {
    return fail("Equal sources did not share one prepared document");
  }
  std::lock_guard<std::mutex> lock(preparedMutex);
  RenderPreparer::install(nullptr);
  size_t calls = 0;
  for (const auto &[id, hint] : preparedCalls) {
    calls += id == first->result().document->id;
  }
  return calls == 2 || fail("Prepared document was not prepared at the recent widths");
}

// Many streams share the bounded pool, and each ends with its own document.
bool sessionsShareThePool() {
  constexpr int kSessions = 16;
  std::mutex mutex;
  std::condition_variable changed;
  std::set<std::thread::id> threads;
  std::vector<bool> complete(kSessions, false);
  for (int index = 0; index < kSessions; index++) {
    const std::string id = "pool-" + std::to_string(index);
    if (!SessionRegistry::create(id, index % 2 ? ParseMode::FullParse : ParseMode::IncrementalTail,
                                 [&, index](const SessionEvent &event) {
                                   std::lock_guard<std::mutex> lock(mutex);
                                   threads.insert(std::this_thread::get_id());
                                   if (event.status == ParseStatus::Ok && event.sequence == 40) {
                                     complete[index] = true;
                                     changed.notify_all();
                                   }
                                 })) {
      return fail("Could not create a pooled session");
    }
  }
  for (uint64_t sequence = 1; sequence <= 40; sequence++) {
    for (int index = 0; index < kSessions; index++) {
      const std::string chunk = "Line " + std::to_string(sequence) + " of stream " + std::to_string(index) + ".\n\n";
      if (!SessionRegistry::append("pool-" + std::to_string(index), chunk, 0, sequence)) {
        return fail("Pooled append failed");
      }
    }
  }
  {
    std::unique_lock<std::mutex> lock(mutex);
    if (!changed.wait_for(lock, 5s, [&] {
          for (bool done : complete) {
            if (!done) return false;
          }
          return true;
        })) {
      return fail("A pooled session did not publish its last append");
    }
  }
  for (int index = 0; index < kSessions; index++) {
    const std::string id = "pool-" + std::to_string(index);
    const auto document = SessionRegistry::snapshot(id, 0, 40);
    if (!document || document->blocks.size() != 40 || document->blocks.back().text.rfind(u"Line 40", 0) != 0) {
      return fail("Pooled session published the wrong document");
    }
    SessionRegistry::dispose(id);
  }
  std::lock_guard<std::mutex> lock(mutex);
  return threads.size() <= WorkerPool::shared().threadCount() ||
      fail("Sessions used more threads than the pool has");
}

// A stream that appends faster than a parse completes still shows progress;
// a pass publishes its result even when newer input arrived meanwhile.
bool fastStreamStillPublishes() {
  const std::string id = "fast-stream";
  std::atomic<int> events{0};
  if (!SessionRegistry::create(id, ParseMode::FullParse, [&](const SessionEvent &event) {
        if (event.status == ParseStatus::Ok) events++;
      })) {
    return fail("Could not create fast session");
  }
  std::string block;
  while (block.size() < 60 * 1024) block += "- **item** with [a link](https://example.com) and `code`\n";
  uint64_t sequence = 0;
  // A large source makes each full parse slower than the append interval.
  for (int i = 0; i < 10; i++) {
    SessionRegistry::append(id, block, 0, ++sequence);
  }
  const auto start = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() - start < 400ms) {
    SessionRegistry::append(id, "more ", 0, ++sequence);
    std::this_thread::sleep_for(200us);
  }
  const int duringAppends = events.load();
  SessionRegistry::dispose(id);
  return duringAppends > 0 || fail("Session published nothing while appends outpaced parsing");
}

} // namespace

int main() {
  return poolRunsJobsInParallel() && waiterRunsQueuedTask() && preparedDocumentsAreSharedAndPrepared() &&
          sessionsShareThePool() && fastStreamStillPublishes()
      ? 0
      : 1;
}
