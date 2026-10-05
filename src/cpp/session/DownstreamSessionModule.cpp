#include "DownstreamSessionModule.h"

#include "FenceSlots.h"
#include "MarkdownParser.h"
#include "PreparedDocument.h"
#include "SessionRegistry.h"
#include "SessionTrace.h"
#include "WorkerPool.h"
#include "math/MathEngine.h"
#include "Unicode.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <limits>
#include <mutex>
#include <utility>

namespace facebook::react {

using SnapshotReadyEvent = NativeDownstreamSessionSnapshotReadyEvent<std::string, int32_t, int32_t>;
using SessionErrorEvent =
    NativeDownstreamSessionSessionErrorEvent<std::string, int32_t, int32_t, std::string, std::string, bool>;
using NativeTraceEvent =
    NativeDownstreamSessionNativeTraceEvent<std::string, int32_t, int32_t, std::string, std::string, std::string>;

template <>
struct Bridging<SnapshotReadyEvent> : NativeDownstreamSessionSnapshotReadyEventBridging<SnapshotReadyEvent> {};

template <>
struct Bridging<SessionErrorEvent> : NativeDownstreamSessionSessionErrorEventBridging<SessionErrorEvent> {};

template <>
struct Bridging<NativeTraceEvent> : NativeDownstreamSessionNativeTraceEventBridging<NativeTraceEvent> {};

/// One fence slot on the wire, in the shape codegen generated for
/// `FenceSlot`: an ordinal, the language as written, the meta string as an
/// optional, the text, and whether the fence is still growing.
using Slot = FenceSlotResult;

/// The one specialization the generated header leaves to the module: how a
/// slot structure becomes a JavaScript object, and back.
template <>
struct Bridging<Slot> : NativeDownstreamSessionFenceSlotBridging<Slot> {};

namespace {

bool validCounter(double value) {
  return std::isfinite(value) && value >= 0 && value <= 9007199254740991.0 && std::floor(value) == value;
}

/// Event counters are Int32 in the spec. Streams never get near the limit;
/// clamp rather than wrap if one does.
int32_t eventCounter(double value) {
  if (!validCounter(value)) {
    return 0;
  }
  return value > std::numeric_limits<int32_t>::max() ? std::numeric_limits<int32_t>::max()
                                                     : static_cast<int32_t>(value);
}

} // namespace

struct DownstreamSessionModule::Events {
  std::mutex mutex;
  DownstreamSessionModule *module{nullptr};
};

DownstreamSessionModule::DownstreamSessionModule(std::shared_ptr<CallInvoker> jsInvoker)
    : NativeDownstreamSessionCxxSpec(std::move(jsInvoker)), events_(std::make_shared<Events>()) {
  events_->module = this;
}

DownstreamSessionModule::~DownstreamSessionModule() {
  {
    std::lock_guard<std::mutex> lock(events_->mutex);
    events_->module = nullptr;
  }
  for (const std::string &sessionId : sessions_) {
    downstream::SessionTrace::disable(sessionId);
    downstream::SessionRegistry::dispose(sessionId);
  }
}

void DownstreamSessionModule::deliverTrace(const downstream::TraceEvent &event) {
  emitOnTrace(NativeTraceEvent{event.sessionId, eventCounter(static_cast<double>(event.generation)),
                               eventCounter(static_cast<double>(event.sequence)), event.event,
                               "native-monotonic", event.timeNs});
}

void DownstreamSessionModule::deliver(const downstream::SessionEvent &event) {
  const auto generation = static_cast<double>(event.generation);
  const auto sequence = static_cast<double>(event.sequence);
  if (event.status == downstream::ParseStatus::Ok) {
    emitOnSnapshotReady(SnapshotReadyEvent{event.sessionId, eventCounter(generation), eventCounter(sequence)});
  } else {
    reportError(event.sessionId, downstream::errorCode(event.status), event.message, generation, sequence);
  }
}

void DownstreamSessionModule::reportError(const std::string &sessionId, const std::string &code,
                                          const std::string &message, double generation, double sequence) {
  emitOnSessionError(
      SessionErrorEvent{sessionId, eventCounter(generation), eventCounter(sequence), code, message, true});
}

void DownstreamSessionModule::create(jsi::Runtime & /*rt*/, std::string sessionId, std::string mode,
                                     double mathFlags) {
  if (sessionId.empty() || (mode != "full" && mode != "incremental")) {
    reportError(sessionId, "invalid_session", "A nonempty session ID and valid parse mode are required", 0, 0);
    return;
  }
  // A bitmask crosses the bridge as a number. Anything negative or above 2^32 is a
  // caller that is not this library, and an unset parse is the safe answer: every
  // formula stays literal rather than one being read as a flag it was not.
  const double validated = std::clamp(mathFlags, 0.0, 4294967295.0);
  const auto mathOptions = downstream::MathOptions{static_cast<uint32_t>(validated)};
  const auto parseMode =
      mode == "incremental" ? downstream::ParseMode::IncrementalTail : downstream::ParseMode::FullParse;
  try {
    // Workers call back on their own threads; the weak reference and lock
    // keep them from reaching a destroyed module.
    std::weak_ptr<Events> weakEvents = events_;
    const bool created = sessions_.count(sessionId) == 0 &&
        downstream::SessionRegistry::create(sessionId, parseMode, [weakEvents](const downstream::SessionEvent &event) {
          const auto events = weakEvents.lock();
          if (!events) {
            return;
          }
          std::lock_guard<std::mutex> lock(events->mutex);
          if (events->module != nullptr) {
            events->module->deliver(event);
          }
        });
    if (!created) {
      reportError(sessionId, "create_failed", "Session already exists or could not be created", 0, 0);
      return;
    }
    sessions_.insert(sessionId);
    // Applied after the session exists, which is the only order that works: a
    // parse queued before this would read the default flags and lay the same
    // source out two different ways.
    downstream::SessionRegistry::setMath(sessionId, mathOptions);
  } catch (const std::exception &error) {
    reportError(sessionId, "create_failed", error.what(), 0, 0);
  }
}

void DownstreamSessionModule::append(jsi::Runtime & /*rt*/, std::string sessionId, std::string chunk,
                                     double generation, double sequence) {
  if (!validCounter(generation) || !validCounter(sequence)) {
    reportError(sessionId, "invalid_sequence", "Generation and sequence must be nonnegative integers", generation,
                sequence);
    return;
  }
  try {
    if (sessions_.count(sessionId) == 0 ||
        !downstream::SessionRegistry::append(sessionId, chunk, static_cast<uint64_t>(generation),
                                             static_cast<uint64_t>(sequence))) {
      reportError(sessionId, "append_failed", "Session is unavailable or append is out of order", generation,
                  sequence);
    }
  } catch (const std::exception &error) {
    reportError(sessionId, "append_failed", error.what(), generation, sequence);
  }
}

void DownstreamSessionModule::reset(jsi::Runtime & /*rt*/, std::string sessionId, std::string source,
                                    double generation) {
  if (!validCounter(generation)) {
    reportError(sessionId, "invalid_generation", "Generation must be a nonnegative integer", generation, 0);
    return;
  }
  try {
    if (sessions_.count(sessionId) == 0 ||
        !downstream::SessionRegistry::reset(sessionId, source, static_cast<uint64_t>(generation))) {
      reportError(sessionId, "reset_failed", "Session is unavailable or generation is stale", generation, 0);
    }
  } catch (const std::exception &error) {
    reportError(sessionId, "reset_failed", error.what(), generation, 0);
  }
}

void DownstreamSessionModule::finish(jsi::Runtime & /*rt*/, std::string sessionId, double generation,
                                     double sequence) {
  if (!validCounter(generation) || !validCounter(sequence)) {
    reportError(sessionId, "invalid_sequence", "Generation and sequence must be nonnegative integers", generation,
                sequence);
    return;
  }
  try {
    if (sessions_.count(sessionId) == 0 ||
        !downstream::SessionRegistry::finish(sessionId, static_cast<uint64_t>(generation),
                                             static_cast<uint64_t>(sequence))) {
      reportError(sessionId, "finish_failed", "Session is unavailable or finish is out of order", generation,
                  sequence);
    }
  } catch (const std::exception &error) {
    reportError(sessionId, "finish_failed", error.what(), generation, sequence);
  }
}

void DownstreamSessionModule::dispose(jsi::Runtime & /*rt*/, std::string sessionId) {
  if (sessions_.erase(sessionId) != 0) {
    downstream::SessionTrace::disable(sessionId);
    downstream::SessionRegistry::dispose(sessionId);
  }
}

void DownstreamSessionModule::setTracing(jsi::Runtime & /*rt*/, std::string sessionId, bool enabled) {
  if (sessions_.count(sessionId) == 0) return;
  if (!enabled) {
    downstream::SessionTrace::disable(sessionId);
    return;
  }
  std::weak_ptr<Events> weakEvents = events_;
  downstream::SessionTrace::enable(sessionId, [weakEvents](const downstream::TraceEvent &event) {
    const auto events = weakEvents.lock();
    if (!events) return;
    std::lock_guard<std::mutex> lock(events->mutex);
    if (events->module) events->module->deliverTrace(event);
  });
}

void DownstreamSessionModule::setPreview(jsi::Runtime & /*rt*/, std::string sessionId, double features) {
  if (!validCounter(features)) {
    reportError(sessionId, "invalid_input", "Preview features must be a nonnegative integer", 0, 0);
    return;
  }
  // A bitmask crosses the bridge as a number. Only the bits the pass defines
  // are honoured, so a stale caller cannot switch on a feature that no longer
  // exists and quietly do nothing.
  // validCounter allows up to 2^53, so the mask is taken in 64 bits: narrowing
  // a double above UINT32_MAX straight to the 32-bit mask is undefined.
  const auto requested = static_cast<uint64_t>(features);
  downstream::remend::PreviewOptions options;
  options.features = static_cast<downstream::remend::PreviewOptions::features_t>(
      requested & downstream::remend::PreviewOptions::allFeatures());
  if (sessions_.count(sessionId) == 0 ||
      !downstream::SessionRegistry::setPreview(sessionId, options)) {
    reportError(sessionId, "invalid_session", "Session is unavailable", 0, 0);
  }
}

bool DownstreamSessionModule::mathAvailable(jsi::Runtime & /*rt*/) {
  // Asked once per process and answered from the engine's own install flag rather
  // than from anything this module knows: the answer changes only when the app's
  // binary does, which is a relaunch, so caching it here would be a second place
  // that could disagree with the layout pass about whether there is an engine.
  return downstream::MathEngine::available();
}

namespace {

/// The size of the source a controlled query parses on the JavaScript thread.
///
/// The same bound layout waits for (`kWaitForPreparedBytes`), which is what
/// makes this the same wait moved earlier rather than a new one: a source
/// larger than this goes to the worker pool, as it already did for layout.
constexpr size_t kSyncSlotBytes = 16 * 1024;

/// One cached answer, with every input that could change it.
struct CachedSlots {
  std::string source;
  std::uint32_t mathFlags{0};
  std::vector<std::string> languages;
  std::vector<Slot> slots;
};

/// Answers keyed by the source, the math flags *and* the registered languages,
/// so a chat that re-renders the same message, or a StrictMode double render,
/// parses nothing twice — and so an app that registers a renderer later reads
/// a different answer rather than the one that renderer-free screen made.
/// Bounded the way `PreparedDocument`'s own cache is: by count, because one
/// chat keeps a screenful of messages and not the whole history.
struct SlotCache {
  std::mutex mutex;
  std::vector<CachedSlots> entries;
  static constexpr size_t kCapacity = 16;
};

SlotCache &slotCache() {
  static auto *instance = new SlotCache();
  return *instance;
}

std::vector<Slot> slotsFor(const downstream::DocumentHandle &document, const std::vector<std::string> &languages,
                           bool live) {
  std::vector<Slot> out;
  if (!document) {
    return out;
  }
  for (const downstream::FenceSlot &slot : downstream::fenceSlots(*document, languages, live)) {
    Slot bridged{static_cast<std::int32_t>(slot.ordinal),
                 slot.language,
                 slot.meta.empty() ? std::nullopt : std::optional<std::string>(slot.meta),
                 downstream::utf16ToUtf8(slot.code),
                 slot.incomplete,
                 static_cast<double>(slot.identity)};
    out.push_back(std::move(bridged));
  }
  return out;
}

jsi::Array toJs(jsi::Runtime &rt, const std::vector<Slot> &slots) {
  jsi::Array array = jsi::Array(rt, slots.size());
  for (size_t index = 0; index < slots.size(); index++) {
    array.setValueAtIndex(rt, index,
                          NativeDownstreamSessionFenceSlotBridging<Slot>::toJs(rt, slots[index], nullptr));
  }
  return array;
}

/// The slots of a controlled source, answering from a finished preparation when
/// one exists and otherwise from a bare parse. Live is true: a controlled
/// string is the whole message, so a fence the writer never closed reads as
/// incomplete until the app settles it, which is Streamdown's rule for a
/// streaming string that has not been finished yet.
std::vector<Slot> controlledSlots(const std::string &source, downstream::MathOptions math,
                                  const std::vector<std::string> &languages) {
  SlotCache &cache = slotCache();
  {
    std::lock_guard<std::mutex> lock(cache.mutex);
    for (const CachedSlots &entry : cache.entries) {
      if (entry.source == source && entry.mathFlags == math.flags && entry.languages == languages) {
        return entry.slots;
      }
    }
  }
  std::vector<Slot> slots;
  if (const auto prepared = downstream::PreparedDocument::completed(source, math)) {
    if (prepared->result().status == downstream::ParseStatus::Ok) {
      slots = slotsFor(prepared->result().document, languages, true);
    }
  } else if (source.size() <= kSyncSlotBytes && downstream::isValidUtf8(source)) {
    // A bare parse: no highlighting, no math layout, no platform preparation,
    // because none of those can change which fences a document has. The
    // platform rendering belongs to the node, which asks for its own hint.
    const downstream::ParseResult result = downstream::parseMarkdown(source, math);
    if (result.status == downstream::ParseStatus::Ok && result.document) {
      // A controlled source is complete the moment it is parsed, so every block
      // settles — the same rule `PreparedDocument` applies.
      const auto count = static_cast<uint32_t>(std::min<size_t>(result.document->blocks.size(),
                                                                 std::numeric_limits<uint32_t>::max()));
      const downstream::DocumentHandle settled = downstream::withSettledBlocks(result.document, count);
      slots = slotsFor(settled, languages, true);
    }
  }
  {
    std::lock_guard<std::mutex> lock(cache.mutex);
    if (cache.entries.size() >= SlotCache::kCapacity) {
      cache.entries.erase(cache.entries.begin());
    }
    cache.entries.push_back(CachedSlots{source, math.flags, languages, std::move(slots)});
  }
  return slots;
}

} // namespace

jsi::Object DownstreamSessionModule::fenceSlots(jsi::Runtime &rt, std::string source, double mathFlags,
                                                std::vector<std::string> languages) {
  const downstream::MathOptions math{static_cast<uint32_t>(std::clamp(mathFlags, 0.0, 4294967295.0))};
  bool pending = false;
  std::vector<Slot> slots;
  try {
    slots = controlledSlots(source, math, languages);
  } catch (const std::exception &) {
    slots.clear();
  }
  // Too large to read here, or unreadable at all: the caller draws code cards
  // and asks again through the asynchronous form, which is what a source whose
  // preparation is still running does today.
  if (slots.empty() && source.size() > kSyncSlotBytes) {
    pending = true;
  }
  auto result = jsi::Object(rt);
  result.setProperty(rt, "pending", pending);
  result.setProperty(rt, "slots", toJs(rt, slots));
  return result;
}

AsyncPromise<std::vector<Slot>> DownstreamSessionModule::fenceSlotsAsync(jsi::Runtime &rt, std::string source,
                                                                          double mathFlags,
                                                                          std::vector<std::string> languages) {
  const downstream::MathOptions math{static_cast<std::uint32_t>(std::clamp(mathFlags, 0.0, 4294967295.0))};
  // The promise is made here, on the JavaScript thread, and settled from the
  // worker that did the parse: `resolve` and `reject` here cross to the
  // JavaScript thread through the call invoker themselves, so nothing about the
  // runtime travels with them and the whole answer is one value on one thread.
  auto promise = AsyncPromise<std::vector<Slot>>(rt, jsInvoker_);
  std::string failure;
  try {
    // The same pool a preparation uses: a big message's fences are read while
    // the rest of the app keeps working, which is the whole reason a large
    // source does not parse here.
    downstream::WorkerPool::shared().post([promise, source = std::move(source), math,
                                          languages = std::move(languages)]() mutable {
      std::vector<Slot> slots;
      std::string message;
      try {
        const downstream::ParseResult result = downstream::parseMarkdown(source, math);
        if (result.status == downstream::ParseStatus::Ok && result.document) {
          // A controlled source is complete the moment it is parsed, so every
          // block settles, exactly as the synchronous form settles them.
          const auto count = static_cast<std::uint32_t>(
              std::min<size_t>(result.document->blocks.size(), std::numeric_limits<std::uint32_t>::max()));
          slots = slotsFor(downstream::withSettledBlocks(result.document, count), languages, true);
        }
      } catch (const std::exception &error) {
        message = error.what();
      }
      if (!message.empty()) {
        promise.reject(Error(message));
        return;
      }
      promise.resolve(std::move(slots));
    });
  } catch (const std::exception &error) {
    // A pool that will not take the job: answer now rather than leaving the
    // view drawing code cards for a question that can no longer be asked.
    failure = error.what();
  }
  if (!failure.empty()) {
    promise.reject(Error(failure));
  }
  return promise;
}

jsi::Array DownstreamSessionModule::sessionFenceSlots(jsi::Runtime &rt, std::string sessionId, double generation,
                                                      double sequence, std::vector<std::string> languages) {
  std::vector<Slot> slots;
  if (!validCounter(generation) || !validCounter(sequence)) {
    return toJs(rt, slots);
  }
  // The snapshot the view is mounting, not the newest one: the props this
  // answer renders beside name that exact version, so a snapshot that arrives
  // mid-render cannot make the view mount slots from a document it never
  // asked for.
  const downstream::DocumentHandle document =
      downstream::SessionRegistry::snapshot(sessionId, static_cast<uint64_t>(generation), static_cast<uint64_t>(sequence));
  if (document) {
    const bool live = !downstream::SessionRegistry::isFinished(sessionId);
    slots = slotsFor(document, languages, live);
  }
  return toJs(rt, slots);
}

} // namespace facebook::react
