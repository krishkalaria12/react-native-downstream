#pragma once

#include "IncrementalParser.h"

#include <cstdint>
#include <functional>
#include <string>

namespace downstream {

/// Published by a session's worker for the current generation and sequence.
struct SessionEvent {
  std::string sessionId;
  uint64_t generation{0};
  uint64_t sequence{0};
  /// Ok when a snapshot for this version is ready. Otherwise the parse failed
  /// and the previous snapshot stays current.
  ParseStatus status{ParseStatus::Ok};
  std::string message;
};

/// Called on the session's worker thread.
using SessionCallback = std::function<void(const SessionEvent &)>;

// Native sessions survive view unmounts. A worker publishes immutable documents
// only for the current generation and sequence.
class SessionRegistry {
public:
  static bool create(const std::string &id, ParseMode mode, SessionCallback callback);
  static bool append(const std::string &id, const std::string &chunk, uint64_t generation, uint64_t sequence);
  static bool reset(const std::string &id, const std::string &source, uint64_t generation);
  static bool finish(const std::string &id, uint64_t generation, uint64_t sequence);
  /// Removes the session without waiting for a parse in progress. Its worker
  /// releases the session's memory when that parse ends, and publishes no
  /// further events.
  static void dispose(const std::string &id);
  static DocumentHandle snapshot(const std::string &id, uint64_t generation, uint64_t sequence);
};

} // namespace downstream
