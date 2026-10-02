// Portable tests for caching failures, eviction, and engine replacement.
#include "math/MathEngine.h"
#include "tests/check.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace downstream;

namespace {
thread_local char lastMessage[256];
std::mutex gateMutex;
std::condition_variable gate;
bool blockedStarted = false;
bool releaseBlocked = false;
int parallelStarted = 0;
std::atomic<bool> parallelTimedOut{false};
int resolverCalls = 0;

char *copy(const char *value) {
  const size_t size = std::strlen(value) + 1;
  char *buffer = static_cast<char *>(std::malloc(size));
  std::memcpy(buffer, value, size);
  return buffer;
}

const char *lastError() { return lastMessage; }
void freeResult(char *value) {
  // Free is also an engine call and may replace a thread-local error message.
  std::strcpy(lastMessage, "message overwritten by free");
  std::free(value);
}

MathEngineResult reject(const char *tex, const MathEngineOptions *options) {
  if (std::strcmp(tex, "blocked") == 0) {
    std::unique_lock<std::mutex> lock(gateMutex);
    blockedStarted = true;
    gate.notify_all();
    gate.wait(lock, [] { return releaseBlocked; });
  }
  if (std::strcmp(tex, "success") == 0) {
    return {copy("{\"width\":1,\"height\":1,\"depth\":0,\"items\":[]}"), 0};
  }
  if (std::strcmp(tex, "malformed") == 0) {
    return {copy("{\"items\":false}"), 0};
  }
  if (std::strcmp(tex, "silent") == 0) {
    lastMessage[0] = '\0';
    return {nullptr, 1};
  }
  std::snprintf(lastMessage, sizeof(lastMessage), "Rejected %s, display=%d", tex, options->displayMode);
  return {copy("unused failed result"), 1};
}

MathEngineResult accept(const char *, const MathEngineOptions *) {
  return {copy("{\"width\":2,\"height\":1,\"depth\":0,\"items\":[]}"), 0};
}

MathEngineResult rejectInParallel(const char *tex, const MathEngineOptions *options) {
  {
    std::unique_lock<std::mutex> lock(gateMutex);
    parallelStarted++;
    gate.notify_all();
    if (!gate.wait_for(lock, std::chrono::seconds(3), [] { return parallelStarted == 8; })) {
      parallelTimedOut = true;
    }
  }
  return reject(tex, options);
}

MathEngineFunctions rejecting() { return {reject, freeResult, lastError}; }
MathEngineFunctions accepting() { return {accept, freeResult, lastError}; }

bool resolveReject(MathEngineFunctions *out) {
  resolverCalls++;
  *out = rejecting();
  return true;
}
bool resolveAccept(MathEngineFunctions *out) {
  resolverCalls++;
  *out = accepting();
  return true;
}

void failureReuse() {
  MathEngine::install(rejecting(), "fonts");
  MathEngine::clearCache();
  std::string original;
  CHECK(!MathEngine::layout("unfinished", false, &original), "invalid TeX fails");
  CHECK(original == "Rejected unfinished, display=0", "error is copied before free overwrites it");
  for (int i = 0; i < 20; i++) {
    std::string error;
    CHECK(!MathEngine::layout("unfinished", false, &error) && error == original,
          "a cached failure preserves the original message");
  }
  CHECK(MathEngine::engineCallCount() == 1 && MathEngine::cachedCount() == 1,
        "identical failures call the engine once and occupy one entry");
  std::string existing = "a caller's earlier error";
  CHECK(!MathEngine::layout("unfinished", false, &existing) && existing == "a caller's earlier error",
        "a cache hit preserves a caller's nonempty error");
  CHECK(!MathEngine::layout("unfinished", false, nullptr), "cached failures allow a null error pointer");
  std::string displayed;
  CHECK(!MathEngine::layout("unfinished", true, &displayed) && displayed != original,
        "display and inline failures use separate entries");
  for (int i = 1; i <= 10; i++) {
    CHECK(!MathEngine::layout(std::string(i, 'x') + "{", false, nullptr),
          "each growing incomplete formula is evaluated separately");
  }
  CHECK(MathEngine::engineCallCount() == 12, "new TeX prefixes are not mistaken for cached failures");

  std::string silent;
  CHECK(!MathEngine::layout("silent", false, &silent) && !silent.empty(), "missing engine messages get a fallback");
  std::string silentAgain;
  CHECK(!MathEngine::layout("silent", false, &silentAgain) && silentAgain == silent,
        "the fallback message is cached too");

  std::string malformed;
  CHECK(!MathEngine::layout("malformed", false, &malformed) && !malformed.empty(),
        "decoder failures return their reason");
  const uint64_t calls = MathEngine::engineCallCount();
  std::string malformedAgain;
  CHECK(!MathEngine::layout("malformed", false, &malformedAgain) && malformedAgain == malformed,
        "a repeated decoder failure preserves its reason");
  CHECK(MathEngine::engineCallCount() == calls, "rejected display lists are not decoded repeatedly");
}

void invalidation() {
  MathEngine::clearCache();
  CHECK(!MathEngine::layout("unfinished", false, nullptr), "a failure is cached before clear");
  MathEngine::clearCache();
  CHECK(MathEngine::cachedCount() == 0, "clear removes failure entries");
  CHECK(!MathEngine::layout("unfinished", false, nullptr) && MathEngine::engineCallCount() == 1,
        "a cleared failure reaches the engine again");
  const auto old = MathEngine::layout("success", false, nullptr);
  MathEngine::install(accepting(), "other fonts");
  CHECK(MathEngine::cachedCount() == 0, "engine installation removes prior successes and failures");
  const auto replacement = MathEngine::layout("success", false, nullptr);
  CHECK(old && old->width == 1 && replacement && replacement->width == 2 && old != replacement,
        "old shared layouts remain valid while a replacement engine supplies new layouts");
  CHECK(MathEngine::layout("unfinished", false, nullptr), "a previous engine's failure cannot poison its replacement");

  MathEngine::install({}, "");
  MathEngine::installResolver(resolveReject, "first");
  MathEngine::installResolver(resolveAccept, "second");
  resolverCalls = 0;
  CHECK(MathEngine::layout("unfinished", false, nullptr), "a replaced pending resolver uses the new engine");
  CHECK(resolverCalls == 1 && MathEngine::fontDirectory() == "second", "only the replacement resolver runs");
}

void boundedLru() {
  MathEngine::install(rejecting(), "fonts");
  MathEngine::clearCache();
  for (int i = 0; i < 2000; i++) {
    MathEngine::layout("bad" + std::to_string(i), false, nullptr);
  }
  CHECK(MathEngine::cachedCount() == 2000, "failures use the same bounded cache as successes");
  MathEngine::layout("bad0", false, nullptr);
  MathEngine::layout("bad2000", false, nullptr);
  CHECK(MathEngine::cachedCount() == 2000, "failure insertion never exceeds the cache bound");
  const uint64_t calls = MathEngine::engineCallCount();
  MathEngine::layout("bad0", false, nullptr);
  CHECK(MathEngine::engineCallCount() == calls, "a recently used failure survives eviction");
  MathEngine::layout("bad1", false, nullptr);
  CHECK(MathEngine::engineCallCount() == calls + 1, "the least recently used failure is evaluated after eviction");
}

void inFlightInvalidation(bool replaceEngine) {
  MathEngine::install(rejecting(), "fonts");
  MathEngine::clearCache();
  {
    std::lock_guard<std::mutex> lock(gateMutex);
    blockedStarted = false;
    releaseBlocked = false;
  }
  std::string oldError;
  MathLayout::Handle oldResult;
  std::thread worker([&] { oldResult = MathEngine::layout("blocked", false, &oldError); });
  {
    std::unique_lock<std::mutex> lock(gateMutex);
    gate.wait(lock, [] { return blockedStarted; });
  }
  if (replaceEngine) {
    MathEngine::install(accepting(), "replacement");
  } else {
    MathEngine::clearCache();
  }
  {
    std::lock_guard<std::mutex> lock(gateMutex);
    releaseBlocked = true;
  }
  gate.notify_all();
  worker.join();
  CHECK(!oldResult && oldError == "Rejected blocked, display=0", "an in-flight caller still receives its engine's result");
  CHECK(MathEngine::cachedCount() == 0, "in-flight old results cannot refill an invalidated cache");
  if (replaceEngine) {
    CHECK(MathEngine::layout("blocked", false, nullptr), "an in-flight old failure cannot poison a replacement engine");
  } else {
    CHECK(!MathEngine::layout("blocked", false, nullptr) && MathEngine::engineCallCount() == 1,
          "a formula cleared during execution is evaluated again");
  }
}

void parallel() {
  MathEngine::install({rejectInParallel, freeResult, lastError}, "fonts");
  MathEngine::clearCache();
  std::vector<std::thread> threads;
  std::atomic<int> wrong{0};
  for (int t = 0; t < 8; t++) {
    threads.emplace_back([t, &wrong] {
      const std::string tex = "parallel" + std::to_string(t);
      std::string error;
      if (MathEngine::layout(tex, false, &error) || error != "Rejected " + tex + ", display=0") wrong++;
      for (int i = 0; i < 20; i++) {
        std::string again;
        if (MathEngine::layout(tex, false, &again) || again != error) wrong++;
      }
    });
  }
  for (auto &thread : threads) thread.join();
  CHECK(wrong == 0, "parallel failure hits keep each formula's error");
  CHECK(!parallelTimedOut, "independent engine calls execute outside the cache mutex");
  CHECK(MathEngine::engineCallCount() == 8, "each parallel formula's later snapshots reuse its failure");
}
} // namespace

int main() {
  failureReuse();
  invalidation();
  boundedLru();
  inFlightInvalidation(false);
  inFlightInvalidation(true);
  parallel();
  return ds_check_result();
}
