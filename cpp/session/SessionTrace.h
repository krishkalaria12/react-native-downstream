#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
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
      it = it->second.sessionId == sessionId ? state.documents.erase(it) : std::next(it);
    }
  }

  static bool enabled(const std::string &sessionId) {
    auto &state = instance();
    if (!state.anyEnabled.load(std::memory_order_relaxed)) return false;
    std::lock_guard<std::mutex> lock(state.mutex);
    return state.callbacks.count(sessionId) != 0;
  }

  static void bindDocument(uint64_t documentId, const std::string &sessionId, uint64_t generation,
                           uint64_t sequence) {
    auto &state = instance();
    if (!state.anyEnabled.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.callbacks.count(sessionId)) {
      state.documents[documentId] = {sessionId, generation, sequence};
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
    callback({sessionId, generation, sequence, event, std::to_string(ns)});
  }

  static void recordDocument(uint64_t documentId, const char *event) {
    if (!instance().anyEnabled.load(std::memory_order_relaxed)) return;
    Key key;
    {
      auto &state = instance();
      std::lock_guard<std::mutex> lock(state.mutex);
      const auto it = state.documents.find(documentId);
      if (it == state.documents.end()) return;
      key = it->second;
    }
    record(key.sessionId, key.generation, key.sequence, event);
  }

  static bool enabledDocument(uint64_t documentId) {
    if (!instance().anyEnabled.load(std::memory_order_relaxed)) return false;
    auto &state = instance();
    std::lock_guard<std::mutex> lock(state.mutex);
    return state.documents.count(documentId) != 0;
  }

private:
  struct Key {
    std::string sessionId;
    uint64_t generation;
    uint64_t sequence;
  };
  struct State {
    std::mutex mutex;
    std::atomic<bool> anyEnabled{false};
    std::unordered_map<std::string, Callback> callbacks;
    std::unordered_map<uint64_t, Key> documents;
  };
  static State &instance() {
    static State state;
    return state;
  }
};

} // namespace downstream
