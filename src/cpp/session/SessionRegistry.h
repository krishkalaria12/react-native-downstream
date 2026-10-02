#pragma once

#include "IncrementalParser.h"
#include "RenderPreparer.h"

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

/// Called on a worker pool thread, one event at a time per session.
using SessionCallback = std::function<void(const SessionEvent &)>;

// Native sessions survive view unmounts. Sessions share the WorkerPool: each
// parse pass publishes an immutable document for the current generation, and
// prepares its platform rendering first so Fabric finds it ready.
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
  /// Records the width and font scale the session's view was measured at, so
  /// later snapshots are prepared for that layout.
  static void noteLayout(const std::string &id, LayoutHint hint);
  /// Replaces the session's provisional-styling policy. An empty option set
  /// turns the pass off, which leaves every open span literal. Takes effect on
  /// the next pass, which this queues, so it applies even when no new input has
  /// arrived. Returns false when the session does not exist.
  static bool setPreview(const std::string &id, remend::PreviewOptions options);
  /**
   * Replaces the delimiters the session's parse recognizes. Takes effect on the
   * next pass, which this queues.
   *
   * Set once, right after `create`, rather than as an argument to it: the session
   * already exists by the time any input can arrive, so a separate call has the
   * same guarantee and one less thing the create path has to get right. Returns
   * false when the session does not exist.
   */
  static bool setMath(const std::string &id, MathOptions math);
};

} // namespace downstream
