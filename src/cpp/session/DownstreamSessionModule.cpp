#include "DownstreamSessionModule.h"

#include "MarkdownParser.h"
#include "SessionRegistry.h"
#include "SessionTrace.h"
#include "math/MathEngine.h"

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

} // namespace facebook::react
