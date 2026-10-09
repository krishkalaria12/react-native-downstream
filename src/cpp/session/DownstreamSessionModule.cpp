#include "DownstreamSessionModule.h"

#include "Clipboard.h"
#include "ControlledSlots.h"
#include "PreparedDocument.h"
#include "FenceSlots.h"
#include "TagSlots.h"
#include "MarkdownParser.h"
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
                                     double parseFlags) {
  if (sessionId.empty() || (mode != "full" && mode != "incremental")) {
    reportError(sessionId, "invalid_session", "A nonempty session ID and valid parse mode are required", 0, 0);
    return;
  }
  // A bitmask crosses the bridge as a number. Anything negative or above 2^32 is a
  // caller that is not this library, and an unset parse is the safe answer: every
  // formula stays literal rather than one being read as a flag it was not.
  const double validated = std::clamp(parseFlags, 0.0, 4294967295.0);
  const auto mathOptions = downstream::ParseOptions{static_cast<uint32_t>(validated)};
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
    downstream::SessionRegistry::setParse(sessionId, mathOptions);
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

void DownstreamSessionModule::setParseTags(jsi::Runtime & /*rt*/, std::string sessionId,
                                          std::vector<std::string> tags) {
  try {
    if (sessions_.count(sessionId) == 0 ||
        !downstream::SessionRegistry::setParseTags(sessionId, downstream::tagPolicyFromStrings(tags))) {
      reportError(sessionId, "invalid_session", "Session is unavailable", 0, 0);
    }
  } catch (const std::exception &error) {
    reportError(sessionId, "invalid_session", error.what(), 0, 0);
  }
}

bool DownstreamSessionModule::mathAvailable(jsi::Runtime & /*rt*/) {
  // Asked once per process and answered from the engine's own install flag rather
  // than from anything this module knows: the answer changes only when the app's
  // binary does, which is a relaunch, so caching it here would be a second place
  // that could disagree with the layout pass about whether there is an engine.
  return downstream::MathEngine::available();
}

AsyncPromise<bool> DownstreamSessionModule::copyText(jsi::Runtime &rt, std::string text) {
  // The promise is made here, on the JavaScript thread, and settled immediately:
  // both platforms write synchronously, so there is no work to hand to the pool
  // and a reader who taps "Copy link" should not wait on a hop to find out.
  AsyncPromise<bool> promise(rt, jsInvoker_);
  promise.resolve(downstream::copyTextToClipboard(text));
  return promise;
}

namespace {

/// The engine's slots in the shape codegen bridges to JavaScript.
Slot bridged(const downstream::FenceSlot &slot) {
  return Slot{static_cast<std::int32_t>(slot.ordinal), slot.language,
              slot.meta.empty() ? std::nullopt : std::optional<std::string>(slot.meta),
              downstream::utf16ToUtf8(slot.code), slot.incomplete, static_cast<double>(slot.identity)};
}

std::vector<Slot> bridged(const std::vector<downstream::FenceSlot> &slots) {
  std::vector<Slot> out;
  out.reserve(slots.size());
  for (const auto &slot : slots) out.push_back(bridged(slot));
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

/// A tag slot as a plain JavaScript object, built by hand rather than through
/// the generated struct bridging: the attributes are a nested array, which the
/// generated bridging cannot express, so the shape is spelled out here where a
/// reader can see exactly what crosses.
jsi::Object tagSlotToJs(jsi::Runtime &rt, const downstream::TagSlot &slot) {
  jsi::Object object(rt);
  object.setProperty(rt, "container", static_cast<double>(slot.container));
  object.setProperty(rt, "tag", static_cast<double>(slot.tag));
  object.setProperty(rt, "name", jsi::String::createFromUtf8(rt, slot.name));
  jsi::Array attributes(rt, slot.attributes.size());
  for (size_t index = 0; index < slot.attributes.size(); index++) {
    jsi::Array pair(rt, 2);
    pair.setValueAtIndex(rt, 0, jsi::String::createFromUtf8(rt, slot.attributes[index].first));
    pair.setValueAtIndex(rt, 1, jsi::String::createFromUtf8(rt, slot.attributes[index].second));
    attributes.setValueAtIndex(rt, index, std::move(pair));
  }
  object.setProperty(rt, "attributes", std::move(attributes));
  object.setProperty(rt, "markdown", jsi::String::createFromUtf8(rt, slot.markdown));
  object.setProperty(rt, "incomplete", slot.incomplete);
  return object;
}

jsi::Array toJs(jsi::Runtime &rt, const std::vector<downstream::TagSlot> &slots) {
  jsi::Array array = jsi::Array(rt, slots.size());
  for (size_t index = 0; index < slots.size(); index++) {
    array.setValueAtIndex(rt, index, tagSlotToJs(rt, slots[index]));
  }
  return array;
}

/// The tag slots a source holds: answered from a finished preparation when one
/// exists and otherwise from a bare parse, like `parsedFenceSlots`. No cache of
/// its own — the fence cache is what the sync path keyed on, and a source that
/// is prepared is a source the preparation cache covers. A controlled string
/// is the whole message, so an unclosed tag reads as incomplete, which is
/// Streamdown's rule for a streaming string that has not finished.
std::vector<downstream::TagSlot> parsedTagSlots(const std::string &source,
                                                downstream::ParseOptions parse,
                                                const std::vector<std::string> &names) {
  if (names.empty()) {
    return {};
  }
  if (const auto prepared = downstream::PreparedDocument::completed(source, parse)) {
    if (prepared->result().status == downstream::ParseStatus::Ok && prepared->result().document) {
      return downstream::tagSlots(*prepared->result().document, source, names, true,
                                  static_cast<uint32_t>(prepared->result().document->blocks.size()));
    }
  }
  const downstream::ParseResult result = downstream::parseMarkdown(source, parse);
  if (result.status != downstream::ParseStatus::Ok || !result.document) {
    return {};
  }
  return downstream::tagSlots(*result.document, source, names, true,
                              static_cast<uint32_t>(result.document->blocks.size()));
}

} // namespace

jsi::Array DownstreamSessionModule::tagSlots(jsi::Runtime &rt, std::string source, double parseFlags,
                                             std::vector<std::string> names, std::vector<std::string> tags) {
  downstream::ParseOptions parse{static_cast<uint32_t>(std::clamp(parseFlags, 0.0, 4294967295.0))};
  parse.tags = downstream::tagPolicyFromStrings(tags);
  try {
    return toJs(rt, parsedTagSlots(source, parse, names));
  } catch (const std::exception &) {
    // Unreadable source, same rule as fence slots: an empty answer rather than
    // a torn render.
    return jsi::Array(rt, 0);
  }
}

jsi::Array DownstreamSessionModule::sessionTagSlots(jsi::Runtime &rt, std::string sessionId, double generation,
                                                    double sequence, std::vector<std::string> names) {
  if (!validCounter(generation) || !validCounter(sequence)) {
    return jsi::Array(rt, 0);
  }
  const auto snapshot = downstream::SessionRegistry::publishedSnapshot(
      sessionId, static_cast<uint64_t>(generation), static_cast<uint64_t>(sequence), false);
  if (!snapshot.document || !snapshot.tagBodies) {
    return jsi::Array(rt, 0);
  }
  std::vector<downstream::TagSlot> slots;
  for (const auto &slot : downstream::sharedTagSlots(*snapshot.document, {}, names, snapshot.live, {},
                                                   nullptr, snapshot.tagBodies.get())) slots.push_back(*slot);
  return toJs(rt, slots);
}

jsi::Object DownstreamSessionModule::fenceSlots(jsi::Runtime &rt, std::string source, double mathFlags,
                                                std::vector<std::string> languages) {
  const downstream::ParseOptions parse{static_cast<uint32_t>(std::clamp(mathFlags, 0.0, 4294967295.0))};
  downstream::ControlledSlots answer;
  try {
    answer = downstream::controlledFenceSlots(source, parse, languages);
  } catch (const std::exception &) {
    // Unreadable: code cards, and the same `pending` rule a source with no
    // answer gets, so a large one is still asked for on the worker.
    answer = downstream::ControlledSlots{source.size() > downstream::kSyncSlotBytes, {}};
  }
  auto result = jsi::Object(rt);
  result.setProperty(rt, "pending", answer.pending);
  result.setProperty(rt, "slots", toJs(rt, bridged(answer.slots)));
  return result;
}

AsyncPromise<std::vector<Slot>> DownstreamSessionModule::fenceSlotsAsync(jsi::Runtime &rt, std::string source,
                                                                          double mathFlags,
                                                                          std::vector<std::string> languages) {
  const downstream::ParseOptions parse{static_cast<std::uint32_t>(std::clamp(mathFlags, 0.0, 4294967295.0))};
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
    downstream::WorkerPool::shared().post([promise, source = std::move(source), parse,
                                          languages = std::move(languages)]() mutable {
      std::vector<Slot> slots;
      std::string message;
      try {
        // Reuse a finished preparation or slot answer before parsing on the worker.
        slots = bridged(downstream::resolvedFenceSlots(source, parse, languages));
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
  const auto snapshot = downstream::SessionRegistry::publishedSnapshot(
      sessionId, static_cast<uint64_t>(generation), static_cast<uint64_t>(sequence), false);
  if (snapshot.document) {
    slots = bridged(downstream::fenceSlots(*snapshot.document, languages, snapshot.live));
  }
  return toJs(rt, slots);
}


jsi::Object DownstreamSessionModule::sessionFenceSlotChanges(jsi::Runtime &rt, std::string sessionId,
    double generation, double sequence, std::vector<std::string> languages, double baseToken) {
  downstream::SlotChanges<downstream::FenceSlot> changes;
  if (validCounter(generation) && validCounter(sequence) && validCounter(baseToken)) {
    changes = downstream::SessionRegistry::fenceSlotChanges(sessionId, static_cast<uint64_t>(generation),
        static_cast<uint64_t>(sequence), languages, static_cast<uint64_t>(baseToken));
  }
  jsi::Object result(rt);
  result.setProperty(rt, "token", static_cast<double>(changes.token));
  jsi::Array order(rt, changes.order.size());
  for (size_t i = 0; i < changes.order.size(); ++i) order.setValueAtIndex(rt, i, static_cast<double>(changes.order[i]));
  result.setProperty(rt, "order", std::move(order));
  jsi::Array slots(rt, changes.changed.size());
  for (size_t i = 0; i < changes.changed.size(); ++i) {
    slots.setValueAtIndex(rt, i, NativeDownstreamSessionFenceSlotBridging<Slot>::toJs(
        rt, bridged(*changes.changed[i]), nullptr));
  }
  result.setProperty(rt, "changed", std::move(slots));
  return result;
}

jsi::Object DownstreamSessionModule::sessionTagSlotChanges(jsi::Runtime &rt, std::string sessionId,
    double generation, double sequence, std::vector<std::string> names, double baseToken) {
  downstream::SlotChanges<downstream::TagSlot> changes;
  if (validCounter(generation) && validCounter(sequence) && validCounter(baseToken)) {
    changes = downstream::SessionRegistry::tagSlotChanges(sessionId, static_cast<uint64_t>(generation),
        static_cast<uint64_t>(sequence), names, static_cast<uint64_t>(baseToken));
  }
  jsi::Object result(rt);
  result.setProperty(rt, "token", static_cast<double>(changes.token));
  jsi::Array order(rt, changes.order.size());
  for (size_t i = 0; i < changes.order.size(); ++i) order.setValueAtIndex(rt, i, static_cast<double>(changes.order[i]));
  result.setProperty(rt, "order", std::move(order));
  jsi::Array slots(rt, changes.changed.size());
  for (size_t i = 0; i < changes.changed.size(); ++i) slots.setValueAtIndex(rt, i, tagSlotToJs(rt, *changes.changed[i]));
  result.setProperty(rt, "changed", std::move(slots));
  return result;
}


} // namespace facebook::react
