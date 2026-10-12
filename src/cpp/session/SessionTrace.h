#pragma once

#include "Document.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iterator>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

namespace downstream {

struct TraceEvent {
  std::string sessionId;
  uint64_t generation;
  uint64_t sequence;
  std::string event;
  std::string timeNs;
  /// Opaque process-local thread ID, not an operating-system TID.
  std::string threadId;
};

/// A process-local, opt-in trace registry shared by the session worker and
/// Fabric. A document ID keeps later measurement and mount events correlated
/// with the exact snapshot that produced them.
class SessionTrace {
public:
  using Callback = std::function<void(const TraceEvent &)>;

  static void enable(const std::string &sessionId, Callback callback) {
    auto &state = instance();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.callbacks[sessionId] = std::move(callback);
    state.anyEnabled.store(true, std::memory_order_relaxed);
  }

  static void disable(const std::string &sessionId) {
    auto &state = instance();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.callbacks.erase(sessionId);
    state.anyEnabled.store(!state.callbacks.empty(), std::memory_order_relaxed);
    for (auto it = state.documents.begin(); it != state.documents.end();) {
      it = it->second.key.sessionId == sessionId ? state.documents.erase(it) : std::next(it);
    }
    state.pruneThreshold = std::max<size_t>(64, state.documents.size() * 2);
  }

  static bool enabled(const std::string &sessionId) {
    auto &state = instance();
    if (!state.anyEnabled.load(std::memory_order_relaxed)) return false;
    std::lock_guard<std::mutex> lock(state.mutex);
    return state.callbacks.count(sessionId) != 0;
  }

  static void bindDocument(const DocumentHandle &document, const std::string &sessionId, uint64_t generation,
                           uint64_t sequence) {
    if (!document) return;
    auto &state = instance();
    if (!state.anyEnabled.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.callbacks.count(sessionId)) {
      state.documents[document->id] = {{sessionId, generation, sequence}, document};
      if (state.documents.size() >= state.pruneThreshold) {
        for (auto it = state.documents.begin(); it != state.documents.end();) {
          it = it->second.document.expired() ? state.documents.erase(it) : std::next(it);
        }
        state.pruneThreshold = std::max<size_t>(64, state.documents.size() * 2);
      }
    }
  }

  static void record(const std::string &sessionId, uint64_t generation, uint64_t sequence,
                     const char *event) {
    if (!instance().anyEnabled.load(std::memory_order_relaxed)) return;
    Callback callback;
    {
      auto &state = instance();
      std::lock_guard<std::mutex> lock(state.mutex);
      const auto it = state.callbacks.find(sessionId);
      if (it == state.callbacks.end()) return;
      callback = it->second;
    }
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
    // Allocate an ID only on the first enabled event from this thread. Repeated
    // measurements of one snapshot can overlap on Fabric and the mount thread.
    thread_local const auto threadId = std::to_string(
        instance().nextThreadId.fetch_add(1, std::memory_order_relaxed));
    callback({sessionId, generation, sequence, event, std::to_string(ns), threadId});
  }

  static void recordDocument(uint64_t documentId, const char *event) {
    if (!instance().anyEnabled.load(std::memory_order_relaxed)) return;
    Key key;
    {
      auto &state = instance();
      std::lock_guard<std::mutex> lock(state.mutex);
      const auto it = state.documents.find(documentId);
      if (it == state.documents.end()) return;
      if (it->second.document.expired()) {
        // Android can release the old Fabric state before its view reports
        // that a pending frame was superseded.
        if (std::strcmp(event, "frame_unavailable") == 0) {
          key = it->second.key;
          state.documents.erase(it);
          // Deliver below, outside the registry lock.
        } else {
          state.documents.erase(it);
          return;
        }
      } else {
        key = it->second.key;
      }
    }
    record(key.sessionId, key.generation, key.sequence, event);
  }

  static bool enabledDocument(uint64_t documentId) {
    if (!instance().anyEnabled.load(std::memory_order_relaxed)) return false;
    auto &state = instance();
    std::lock_guard<std::mutex> lock(state.mutex);
    const auto it = state.documents.find(documentId);
    if (it == state.documents.end()) return false;
    if (it->second.document.expired()) {
      state.documents.erase(it);
      return false;
    }
    return true;
  }

  /// Diagnostic count for validating that trace metadata follows live documents.
  static size_t trackedDocumentCount() {
    auto &state = instance();
    std::lock_guard<std::mutex> lock(state.mutex);
    return state.documents.size();
  }

private:
  struct Key {
    std::string sessionId;
    uint64_t generation;
    uint64_t sequence;
  };
  struct DocumentEntry {
    Key key;
    std::weak_ptr<const Document> document;
  };
  struct State {
    std::mutex mutex;
    std::atomic<bool> anyEnabled{false};
    std::atomic<uint64_t> nextThreadId{1};
    std::unordered_map<std::string, Callback> callbacks;
    std::unordered_map<uint64_t, DocumentEntry> documents;
    size_t pruneThreshold{64};
  };
  static State &instance() {
    static State state;
    return state;
  }
};

} // namespace downstream
