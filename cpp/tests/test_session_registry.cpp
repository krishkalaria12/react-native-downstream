#include "SessionRegistry.h"
#include "SessionTrace.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

using namespace downstream;

int main() {
  std::mutex mutex;
  std::condition_variable ready;
  std::vector<std::tuple<uint64_t, uint64_t>> published;
  const std::string id = "session-test";
  std::mutex traceMutex;
  std::vector<TraceEvent> traces;
  SessionTrace::enable(id, [&](const TraceEvent &event) {
    std::lock_guard<std::mutex> lock(traceMutex);
    traces.push_back(event);
  });
  const auto callback = [&](const SessionEvent &event) {
    if (event.sessionId != id || event.status != ParseStatus::Ok) {
      return;
    }
    {
      std::lock_guard<std::mutex> lock(mutex);
      published.emplace_back(event.generation, event.sequence);
    }
    ready.notify_one();
  };
  const auto await = [&](uint64_t generation, uint64_t sequence) {
    std::unique_lock<std::mutex> lock(mutex);
    return ready.wait_for(lock, std::chrono::seconds(3), [&] {
      for (const auto &event : published) {
        if (event == std::make_tuple(generation, sequence)) {
          return true;
        }
      }
      return false;
    });
  };

  if (!SessionRegistry::create(id, ParseMode::IncrementalTail, callback) ||
      SessionRegistry::create(id, ParseMode::FullParse, callback) ||
      !SessionRegistry::append(id, "hello", 0, 2) || !await(0, 2)) {
    std::cerr << "Initial native session append failed\n";
    return 1;
  }
  auto first = SessionRegistry::snapshot(id, 0, 2);
  if (!first || first->blocks.size() != 1 || first->blocks[0].text != u"hello" ||
      SessionRegistry::snapshot(id, 0, 1)) {
    std::cerr << "Snapshot version mismatch\n";
    return 1;
  }
  SessionTrace::recordDocument(first->id, "measure_start");
  {
    std::lock_guard<std::mutex> lock(traceMutex);
    bool parsed = false;
    bool measured = false;
    for (const auto &event : traces) {
      if (event.sessionId != id || event.generation != 0 || event.sequence != 2 || event.timeNs.empty()) continue;
      parsed |= event.event == "parse_end";
      measured |= event.event == "measure_start";
    }
    if (!parsed || !measured) {
      std::cerr << "Trace lost the snapshot correlation\n";
      return 1;
    }
  }

  // A published prefix stays paintable while the next batch is pending.
  if (!SessionRegistry::append(id, " world", 0, 4) || !SessionRegistry::snapshot(id, 0, 2) ||
      !await(0, 4)) {
    std::cerr << "Published prefix was lost\n";
    return 1;
  }
  if (!SessionRegistry::reset(id, "new", 1) || SessionRegistry::snapshot(id, 0, 4) ||
      SessionRegistry::append(id, "stale", 0, 5) || !await(1, 0)) {
    std::cerr << "Reset did not invalidate old work\n";
    return 1;
  }
  auto replacement = SessionRegistry::snapshot(id, 1, 0);
  if (!replacement || replacement->blocks.size() != 1 || replacement->blocks[0].text != u"new" ||
      SessionRegistry::reset(id, std::string(1, static_cast<char>(0xff)), 2) ||
      !SessionRegistry::snapshot(id, 1, 0) ||
      !SessionRegistry::finish(id, 1, 0) || SessionRegistry::append(id, "late", 1, 1)) {
    std::cerr << "Finished session accepted another append\n";
    return 1;
  }
  SessionRegistry::dispose(id);
  if (SessionRegistry::snapshot(id, 1, 0) || SessionRegistry::append(id, "late", 1, 1) ||
      SessionTrace::enabled(id) || SessionTrace::enabledDocument(first->id)) {
    std::cerr << "Disposed session remained accessible\n";
    return 1;
  }

  // Dispose returns while a large parse is in flight and no event follows it.
  const std::string busy = "busy-session";
  std::atomic<int> busyEvents{0};
  std::atomic<bool> disposed{false};
  std::atomic<bool> lateEvent{false};
  if (!SessionRegistry::create(busy, ParseMode::FullParse, [&](const SessionEvent &) {
        busyEvents++;
        if (disposed) lateEvent = true;
      })) {
    std::cerr << "Could not create busy session\n";
    return 1;
  }
  std::string chunk;
  while (chunk.size() < 60 * 1024) chunk += "- **item** with [a link](https://example.com) and `code`\n";
  for (uint64_t sequence = 1; sequence <= 12; sequence++) {
    if (!SessionRegistry::append(busy, chunk, 0, sequence)) {
      std::cerr << "Busy append failed\n";
      return 1;
    }
  }
  SessionRegistry::dispose(busy);
  disposed = true;
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  if (lateEvent || SessionRegistry::create(busy, ParseMode::FullParse, callback) == false) {
    std::cerr << "Disposed session published an event or kept its ID\n";
    return 1;
  }
  SessionRegistry::dispose(busy);
  return 0;
}
