#include "SessionRegistry.h"
#include "SessionTrace.h"
#include "check.h"

#include <algorithm>
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
  for (uint64_t index = 1; index <= 200; index++) {
    auto transient = std::make_shared<Document>();
    transient->id = 1000000 + index;
    SessionTrace::bindDocument(transient, id, 0, index);
  }
  if (SessionTrace::trackedDocumentCount() > 64 ||
      SessionTrace::enabledDocument(1000001)) {
    std::cerr << "Trace retained expired snapshot associations\n";
    return 1;
  }
  {
    auto transient = std::make_shared<Document>();
    transient->id = 2000000;
    SessionTrace::bindDocument(transient, id, 0, 3);
  }
  SessionTrace::recordDocument(2000000, "frame_unavailable");
  {
    std::lock_guard<std::mutex> lock(traceMutex);
    if (traces.empty() || traces.back().event != "frame_unavailable" || traces.back().sequence != 3) {
      std::cerr << "Trace dropped the superseded frame marker\n";
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

  // A provisional-styling policy can be set on a running session, takes effect
  // without any new input, and leaves the canonical source alone. Turning it
  // back off restores the literal markers.
  const std::string previewId = "preview-session";
  std::mutex previewMutex;
  std::condition_variable previewReady;
  std::vector<std::tuple<uint64_t, uint64_t>> previewEvents;
  const auto previewCallback = [&](const SessionEvent &event) {
    if (event.sessionId != previewId || event.status != ParseStatus::Ok) {
      return;
    }
    {
      std::lock_guard<std::mutex> lock(previewMutex);
      previewEvents.emplace_back(event.generation, event.sequence);
    }
    previewReady.notify_one();
  };
  const auto awaitPreview = [&](uint64_t generation, uint64_t sequence) {
    std::unique_lock<std::mutex> lock(previewMutex);
    return previewReady.wait_for(lock, std::chrono::seconds(3), [&] {
      return std::find(previewEvents.begin(), previewEvents.end(),
                       std::make_tuple(generation, sequence)) != previewEvents.end();
    });
  };
  // A policy change publishes under the sequence already in flight, so the
  // wait is for one more event rather than for a new pair.
  const auto awaitNextPreview = [&]() {
    std::unique_lock<std::mutex> lock(previewMutex);
    const size_t before = previewEvents.size();
    return previewReady.wait_for(lock, std::chrono::seconds(3), [&] {
      return previewEvents.size() > before;
    });
  };

  CHECK(SessionRegistry::create(previewId, ParseMode::IncrementalTail, previewCallback),
        "create a session to preview on");
  CHECK(SessionRegistry::append(previewId, "hello **bold wor", 0, 1) && awaitPreview(0, 1),
        "append to the preview session");
  const auto literal = SessionRegistry::snapshot(previewId, 0, 1);
  CHECK(literal && literal->blocks.back().text == u"hello **bold wor",
        "an unset policy leaves the markers literal");

  // Unknown features are masked to what the pass defines, so a stale caller
  // cannot ask for something that does not exist and appear to succeed.
  remend::PreviewOptions options;
  options.features = remend::PreviewOptions::allFeatures() | 0x80000000u;
  CHECK(SessionRegistry::setPreview(previewId, options) && awaitNextPreview(),
        "set a policy on a running session");
  const auto styled = SessionRegistry::snapshot(previewId, 0, 1);
  CHECK(styled && styled->blocks.back().text == u"hello bold wor",
        "the policy restyles the tail with no new input");
  CHECK(styled && !styled->blocks.back().runs.empty(), "the restyled tail is styled");
  CHECK(styled && styled->blocks.back().runs.front().styles == InlineStyle::Strong,
        "the span is strong");

  CHECK(SessionRegistry::setPreview(previewId, remend::PreviewOptions{}) && awaitNextPreview(),
        "turn the policy back off");
  const auto again = SessionRegistry::snapshot(previewId, 0, 1);
  CHECK(again && again->blocks.back().text == u"hello **bold wor",
        "turning the policy off restores the literal markers");

  // A finished stream ignores the policy entirely, so the settled document is
  // a parse of the bytes the writer sent. An unclosed span is the case that
  // tells the two apart: previewed it would read as bold, settled it is
  // literal.
  const std::string settledId = "settled-session";
  std::mutex settledMutex;
  std::condition_variable settledReady;
  std::vector<std::tuple<uint64_t, uint64_t>> settledEvents;
  const auto settledCallback = [&](const SessionEvent &event) {
    if (event.sessionId != settledId || event.status != ParseStatus::Ok) {
      return;
    }
    {
      std::lock_guard<std::mutex> lock(settledMutex);
      settledEvents.emplace_back(event.generation, event.sequence);
    }
    settledReady.notify_one();
  };
  const auto awaitSettled = [&]() {
    std::unique_lock<std::mutex> lock(settledMutex);
    const size_t before = settledEvents.size();
    return settledReady.wait_for(lock, std::chrono::seconds(3), [&] {
      return settledEvents.size() > before;
    });
  };

  CHECK(SessionRegistry::create(settledId, ParseMode::IncrementalTail, settledCallback),
        "create a session to settle");
  CHECK(SessionRegistry::setPreview(settledId, remend::PreviewOptions::all()),
        "enable the policy before any input");
  CHECK(SessionRegistry::append(settledId, "hello **bold wor", 0, 1), "append an open span");
  CHECK(awaitSettled(), "the previewed session publishes");
  const auto previewedTail = SessionRegistry::snapshot(settledId, 0, 1);
  CHECK(previewedTail && previewedTail->blocks.back().text == u"hello bold wor",
        "the open span previews as bold while the stream runs");

  CHECK(SessionRegistry::finish(settledId, 0, 1), "finish with the span still open");
  CHECK(awaitSettled(), "the settled session publishes");
  const auto settledTail = SessionRegistry::snapshot(settledId, 0, 1);
  CHECK(settledTail && settledTail->blocks.back().text == u"hello **bold wor",
        "a finished stream is never repaired");
  CHECK(settledTail && settledTail->blocks.back().runs.empty(),
        "a finished stream leaves the open span unstyled");
  SessionRegistry::dispose(settledId);

  CHECK(!SessionRegistry::setPreview("no-such-session", remend::PreviewOptions::all()),
        "an unknown session rejects a policy change");
  SessionRegistry::dispose(previewId);

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
  // This flow returns early on a failed dispose, because continuing would race
  // against a live worker; the earlier failures return with a message above.
  CHECK(ds_check_failures == 0, "session registry lifecycle");
  return ds_check_result();
}
