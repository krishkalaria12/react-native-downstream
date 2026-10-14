#pragma once

#include "IncrementalParser.h"
#include "RenderPreparer.h"
#include "SessionSlotCache.h"

#include <cstdint>
#include <functional>
#include <string>

namespace downstream {

/// Published on a native frame tick after the worker prepares this version.
struct SessionEvent {
  std::string sessionId;
  uint64_t generation{0};
  uint64_t sequence{0};
  /// Ok when a snapshot for this version is ready. Otherwise the parse failed
  /// and the previous snapshot stays current.
  ParseStatus status{ParseStatus::Ok};
  std::string message;
};

/// Source, document, and stream state from the same published parse pass.
struct PublishedSessionSnapshot {
  DocumentHandle document;
  std::shared_ptr<const std::string> source;
  bool live{false};
  std::shared_ptr<const TagSourceBodies> tagBodies;
  std::shared_ptr<const std::vector<TagSlotRange>> tagStructure;
};

/// Called on the native frame driver thread, one event per session per tick.
/// Headless callers use the WorkerPool fallback. Must not reenter the session.
using SessionCallback = std::function<void(const SessionEvent &)>;

// Native sessions survive view unmounts. Sessions share the WorkerPool: each
// parse pass prepares an immutable document on a worker. A native frame tick
// publishes it so Fabric finds rendering ready, coalescing pending input and
// reducing the update rate when preparation or frame delivery falls behind.
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
  static PublishedSessionSnapshot publishedSnapshot(const std::string &id, uint64_t generation, uint64_t sequence, bool includeSource = true);
  /// Whether the writer finished the stream. A finished stream settles every
  /// block, so a fence it never closed is final rather than still arriving.
  static SlotChanges<FenceSlot> fenceSlotChanges(const std::string &id, uint64_t generation, uint64_t sequence,
                                                const std::vector<std::string> &languages, uint64_t baseToken);
  static SlotChanges<TagSlot> tagSlotChanges(const std::string &id, uint64_t generation, uint64_t sequence,
                                            const std::vector<std::string> &names, uint64_t baseToken);
  static SlotQueryWork slotWork(const std::string &id);
  static bool isFinished(const std::string &id);
  /// Records the width and font scale the session's view was measured at, so
  /// later snapshots are prepared for that layout.
  static void noteLayout(const std::string &id, LayoutHint hint);
  /// Replaces the session's provisional-styling policy. An empty option set
  /// turns the pass off, which leaves every open span literal. Takes effect on
  /// the next pass, which this queues, so it applies even when no new input has
  /// arrived. Returns false when the session does not exist.
  static bool setPreview(const std::string &id, remend::PreviewOptions options);
/// Records the document a view last showed for this session, so a view that
  /// binds later — a virtualized list scrolling a live message back into view —
  /// starts its reveal tracker from what was already on screen instead of
  /// fading the whole message in again.
  ///
  /// A reset clears it, so a new generation animates from its first word.
  static void noteShown(const std::string &id, const DocumentHandle &document);
  /// The last recorded shown document, or null.
  static DocumentHandle shown(const std::string &id);
  /**
   * Replaces the delimiters the session's parse recognizes. Takes effect on the
   * next pass, which this queues.
   *
   * Set once, right after `create`, rather than as an argument to it: the session
   * already exists by the time any input can arrive, so a separate call has the
   * same guarantee and one less thing the create path has to get right. Returns
   * false when the session does not exist.
   */
  static bool setParse(const std::string &id, ParseOptions parse);

  /**
   * Replaces the session's HTML tag policy, keeping its flags.
   *
   * A separate call for the same reason `setParse` is: `create` is one crossing
   * with one number in it, and a tag policy is a list. Reading the flags back off
   * the session rather than taking them as an argument is what keeps a caller that
   * only knows about tags from clearing the bits that came with `create`.
   *
   * Returns false when the session does not exist.
   */
  static bool setParseTags(const std::string &id, TagPolicy tags);
};

} // namespace downstream
