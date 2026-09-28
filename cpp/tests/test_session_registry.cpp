#include "SessionRegistry.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>

using namespace downstream;

int main() {
  std::mutex mutex;
  std::condition_variable ready;
  std::vector<std::tuple<uint64_t, uint64_t>> published;
  const std::string id = "session-test";
  const auto callback = [&](const std::string &sessionId, uint64_t generation, uint64_t sequence) {
    if (sessionId != id) {
      return;
    }
    {
      std::lock_guard<std::mutex> lock(mutex);
      published.emplace_back(generation, sequence);
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
  if (SessionRegistry::snapshot(id, 1, 0) || SessionRegistry::append(id, "late", 1, 1)) {
    std::cerr << "Disposed session remained accessible\n";
    return 1;
  }
  return 0;
}
