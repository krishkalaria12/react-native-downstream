#include "SessionRegistry.h"

#include "MarkdownParser.h"
#include "RenderPreparer.h"
#include "SessionTrace.h"
#include "Unicode.h"
#include "WorkerPool.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace downstream {
namespace {

// A session has no thread of its own. While it has unparsed input, one job for
// it is queued on the shared pool; the job parses once, prepares the platform
// rendering, publishes, and queues itself again if more input arrived. Many
// streams therefore share a few cores fairly, and a quiet stream costs nothing.
class Session : public std::enable_shared_from_this<Session> {
public:
  Session(std::string id, ParseMode mode, SessionCallback callback)
      : id_(std::move(id)), parser_(mode), callback_(std::move(callback)) {}

  void stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    stopped_ = true;
    dirty_ = false;
  }

  bool append(const std::string &chunk, uint64_t generation, uint64_t sequence) {
    if (chunk.empty()) {
      return true;
    }
    if (!isValidUtf8(chunk)) {
      return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_ || finished_ || generation != generation_ || sequence <= sequence_ ||
        chunk.size() > 64 * 1024 || sourceBytes_ + chunk.size() > kMaxSourceBytes) {
      return false;
    }
    pending_.append(chunk);
    sourceBytes_ += chunk.size();
    sequence_ = sequence;
    SessionTrace::record(id_, generation, sequence, "append_received");
    markDirty();
    return true;
  }

  bool reset(const std::string &source, uint64_t generation) {
    if (source.size() > kMaxSourceBytes || !isValidUtf8(source)) {
      return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_ || generation <= generation_) {
      return false;
    }
    replacement_ = source;
    pending_.clear();
    sourceBytes_ = source.size();
    generation_ = generation;
    sequence_ = 0;
    finished_ = false;
    SessionTrace::record(id_, generation, 0, "reset_received");
    snapshot_.reset();
    snapshotSource_.reset();
    snapshotTagBodies_.reset();
    snapshotTagStructure_.reset();
    {
      std::lock_guard<std::mutex> slots(slotMutex_);
      fenceCache_.clear();
      tagCache_.clear();
      tagStructure_.clear();
      tagStructureDocument_ = 0;
    }
    // The new source has shown nothing, so a view that binds to it fades from
    // its first word rather than starting from the previous one's tail.
    shown_.reset();
    readyGeneration_ = 0;
    readySequence_ = 0;
    markDirty();
    return true;
  }

  bool finish(uint64_t generation, uint64_t sequence) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_ || generation != generation_ || sequence != sequence_) {
      return false;
    }
    if (!finished_) {
      finished_ = true;
      markDirty();
    }
    return true;
  }

  PublishedSessionSnapshot publishedSnapshot(uint64_t generation, uint64_t sequence, bool includeSource = true) const {
    PublishedSessionSnapshot result;
    std::shared_ptr<const SharedList<std::string>::Prefix> source;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (generation != readyGeneration_ || sequence != readySequence_ || generation != generation_) return {};
      result = {snapshot_, nullptr, snapshotLive_, snapshotTagBodies_, snapshotTagStructure_};
      if (includeSource) source = snapshotSource_;
    }
    // Compatibility consumers request canonical source explicitly. Native slot
    // queries read shared body slices and never flatten the response.
    if (source) {
      std::string joined;
      size_t bytes = 0;
      for (size_t i = 0; i < source->size(); ++i) bytes += (*source)[i].size();
      joined.reserve(bytes);
      for (size_t i = 0; i < source->size(); ++i) joined.append((*source)[i]);
      result.source = std::make_shared<const std::string>(std::move(joined));
    }
    return result;
  }

  DocumentHandle snapshot(uint64_t generation, uint64_t sequence) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return generation == readyGeneration_ && sequence == readySequence_ && generation == generation_
               ? snapshot_
               : nullptr;
  }

  SlotChanges<FenceSlot> fenceSlotChanges(uint64_t generation, uint64_t sequence,
                                         const std::vector<std::string> &languages, uint64_t baseToken) {
    const auto published = publishedSnapshot(generation, sequence, false);
    if (!published.document) return {};
    std::lock_guard<std::mutex> lock(slotMutex_);
    return fenceCache_.query(published.document->id, generation, published.live, languages, baseToken,
        [&](const auto &previous) { return sharedFenceSlots(*published.document, languages, published.live, previous); });
  }

  SlotChanges<TagSlot> tagSlotChanges(uint64_t generation, uint64_t sequence,
                                     const std::vector<std::string> &names, uint64_t baseToken) {
    const auto published = publishedSnapshot(generation, sequence, false);
    if (!published.document || !published.tagBodies) return {};
    std::lock_guard<std::mutex> lock(slotMutex_);
    if (tagStructureDocument_ != published.document->id) {
      tagStructure_ = published.tagStructure ? *published.tagStructure : std::vector<TagSlotRange>{};
      tagStructureDocument_ = published.document->id;
      ++tagStructureBuilds_;
    }
    return tagCache_.query(published.document->id, generation, published.live, names, baseToken,
        [&](const auto &previous) { return sharedTagSlots(*published.document, {}, names, published.live, previous, &tagStructure_, published.tagBodies.get()); });
  }

  SlotQueryWork slotWork() const {
    std::lock_guard<std::mutex> lock(slotMutex_);
    SlotQueryWork result = fenceCache_.work();
    const auto &tags = tagCache_.work();
    result.payloadBuildBytes += tags.payloadBuildBytes;
    result.payloadExportBytes += tags.payloadExportBytes;
    result.reusedPayloads += tags.reusedPayloads;
    result.snapshotBuilds += tags.snapshotBuilds;
    result.retainedBytes += tags.retainedBytes + tagStructure_.size() * sizeof(TagSlotRange);
    result.structureBuilds = tagStructureBuilds_;
    result.sourceCaptureBytes = sourceCaptureBytes_.load(std::memory_order_relaxed);
    result.tagBodyCopyBytes = tagBodyCopyBytes_.load(std::memory_order_relaxed);
    return result;
  }

  /// Whether the writer closed the stream at this generation. The fence-slot
  /// query needs it: a finished stream is settled, so a fence it never closed
  /// is final rather than still growing.
  bool isFinished() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return finished_;
  }

  /// Queues a new provisional-styling policy for the worker. It is applied on
  /// the worker thread, which is the only thread that touches the parser, and
  /// it queues a pass so the new policy reaches the reader without waiting for
  /// the next chunk.
  ///
  /// A finished stream is settled and never previewed, so there is nothing to
  /// repaint. The policy still waits for the worker, and applies from the next
  /// reset.
  void setPreview(remend::PreviewOptions options) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_) {
      return;
    }
    preview_ = options;
    SessionTrace::record(id_, generation_, sequence_, "preview_set");
    if (!finished_) {
      markDirty();
    }
  }

  /// Replaces the delimiters the parse recognizes, and queues a pass so it takes
  /// effect even when no new input has arrived.
  ///
  /// Applied to the parser on the worker rather than here, because the parser is
  /// the only thing that reads the flags and the worker is the only thread that
  /// touches it. Set once, right after `create`, so there is at most one queued
  /// pass between a session existing and its flags being right.
  void setParse(ParseOptions parse) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_) {
      return;
    }
    parse_ = parse;
    parsePending_ = true;
    if (!finished_) {
      markDirty();
    }
  }

  /// The same, for the tag policy alone. See `SessionRegistry::setParseTags`.
  void setParseTags(TagPolicy tags) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_) {
      return;
    }
    parse_.tags = std::move(tags);
    parsePending_ = true;
    if (!finished_) {
      markDirty();
    }
  }

  void noteLayout(LayoutHint hint) {
    std::lock_guard<std::mutex> lock(mutex_);
    layoutHints_.erase(std::remove_if(layoutHints_.begin(), layoutHints_.end(), [&](auto old) {
      const bool sameWidth = old.width == hint.width;
      old.width = hint.width;
      return sameWidth || !(old == hint);
    }), layoutHints_.end());
    layoutHints_.insert(layoutHints_.begin(), std::move(hint));
    if (layoutHints_.size() > 2) layoutHints_.resize(2);
  }

  void noteShown(const DocumentHandle &document) {
    std::lock_guard<std::mutex> lock(mutex_);
    shown_ = document;
  }

  DocumentHandle shown() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return shown_;
  }

private:
  /// Requires mutex_. Queues one job unless one is already queued or running;
  /// a running job sees dirty_ and queues the next pass itself.
  void markDirty() {
    dirty_ = true;
    if (!scheduled_) {
      scheduled_ = true;
      WorkerPool::shared().post([self = shared_from_this()] { self->step(); });
    }
  }

  /// One parse pass. The job holds a reference, so a disposed session lives
  /// until the pass in progress finishes and nobody has to wait for it.
  void step() {
    // Only the bytes accepted since the last pass cross the lock; the parser
    // owns the canonical source.
    std::optional<std::string> replacement;
    std::string pending;
    uint64_t generation;
    uint64_t sequence;
    bool finished;
    std::vector<LayoutHint> hints;
    std::optional<remend::PreviewOptions> preview;
    // The flags travel the same way the preview options do, and for the same
    // reason: only the worker may set them on the parser, and both have to be in
    // place before the parse they change rather than after it.
    std::optional<ParseOptions> parse;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stopped_ || !dirty_) {
        scheduled_ = false;
        return;
      }
      replacement.swap(replacement_);
      pending.swap(pending_);
      preview.swap(preview_);
      if (parsePending_) {
        parse = parse_;
        parsePending_ = false;
      }
      generation = generation_;
      sequence = sequence_;
      finished = finished_;
      dirty_ = false;
      hints = layoutHints_;
    }

    if (preview) {
      parser_.setPreview(*preview);
    }
    if (parse) {
      parser_.setParse(*parse);
    }
    SessionTrace::record(id_, generation, sequence, "parse_start");
    if (replacement) {
      parser_.replace(*replacement);
      tagSource_.reset();
      tagBodyRanges_.clear();
    }
    if (tagSource_ && !pending.empty()) {
      std::vector<std::string> chunk;
      chunk.push_back(pending);
      tagSource_ = SharedList<std::string>::Prefix::extend(tagSource_, std::move(chunk));
      sourceCaptureBytes_.fetch_add(pending.size(), std::memory_order_relaxed);
    }
    parser_.append(pending);
    auto result = parser_.parse(finished);
    SessionTrace::record(id_, generation, sequence, "parse_end");
    const bool parsed = result.status == ParseStatus::Ok && result.document;
    std::shared_ptr<const SharedList<std::string>::Prefix> source;
    std::shared_ptr<const TagSourceBodies> tagBodies;
    std::shared_ptr<const std::vector<TagSlotRange>> tagStructure;
    if (parsed && std::any_of(result.document->tags.begin(), result.document->tags.end(),
                              [](const Tag &tag) { return tag.block; })) {
      // Capture the prefix once when custom tags first appear. Later snapshots
      // share every old chunk and only store bytes accepted by this pass.
      if (!tagSource_) {
        std::vector<std::string> chunk;
        chunk.emplace_back(parser_.source());
        tagSource_ = SharedList<std::string>::Prefix::extend(nullptr, std::move(chunk));
        sourceCaptureBytes_.fetch_add(parser_.source().size(), std::memory_order_relaxed);
      }
      source = tagSource_;
      tagStructure = std::make_shared<const std::vector<TagSlotRange>>(planTagSlots(*result.document));
      std::unordered_set<int32_t> needed;
      for (const auto &range : *tagStructure) needed.insert(range.tag);
      TagSourceBodies bodies;
      std::unordered_map<uint64_t, std::shared_ptr<const std::string>> nextRanges;
      bodies.reserve(result.document->tags.size());
      size_t tagIndex = 0;
      for (const Tag &tag : result.document->tags) {
        if (!needed.count(static_cast<int32_t>(tagIndex++))) {
          static const auto empty = std::make_shared<const std::string>();
          bodies.push_back(empty);
          continue;
        }
        const uint64_t range = (static_cast<uint64_t>(tag.bodyStart) << 32) | tag.bodyEnd;
        const auto old = tagBodyRanges_.find(range);
        auto body = old != tagBodyRanges_.end() ? old->second :
            std::make_shared<const std::string>(tag.block ? tagSourceBody(parser_.source(), tag.bodyStart, tag.bodyEnd)
                                                        : std::string{});
        if (old == tagBodyRanges_.end()) tagBodyCopyBytes_.fetch_add(body->size(), std::memory_order_relaxed);
        bodies.push_back(body);
        if (tag.block && tag.bodyEnd > tag.bodyStart && tag.bodyEnd <= parser_.source().size()) {
          nextRanges.emplace(range, std::move(body));
        }
      }
      tagBodyRanges_ = std::move(nextRanges);
      tagBodies = std::make_shared<const TagSourceBodies>(std::move(bodies));
    }

    if (parsed && isCurrentGeneration(generation)) {
      // The view has not measured yet, so the widths other views use are the
      // best guess for this one.
      // Bind before platform preparation: preparation can emit document-scoped
      // trace events (for example while building Android spans), and those
      // events must resolve to this session's generation and sequence.
      SessionTrace::bindDocument(result.document, id_, generation, sequence);
      SessionTrace::record(id_, generation, sequence, "prepare_start");
      RenderPreparer::prepare(result.document, hints.empty() ? RenderPreparer::recentLayouts() : hints);
      SessionTrace::record(id_, generation, sequence, "prepare_end");
    }

    // The callback runs under the lock so that no event follows stop().
    // It must not call back into this session.
    std::lock_guard<std::mutex> lock(mutex_);
    // A pass publishes even when newer input arrived meanwhile. Waiting for
    // input to pause would show nothing while a stream outpaces parsing; the
    // newer input is parsed by the next pass. Only a reset discards it.
    if (stopped_ || generation != generation_) {
      SessionTrace::record(id_, generation, sequence, "dropped");
    } else {
      if (parsed) {
        snapshot_ = std::move(result.document);
        snapshotSource_ = std::move(source);
        snapshotTagBodies_ = std::move(tagBodies);
        snapshotTagStructure_ = std::move(tagStructure);
        snapshotLive_ = !finished;
        SessionTrace::record(id_, generation, sequence, "snapshot_ready");
        readyGeneration_ = generation;
        readySequence_ = sequence;
      }
      callback_({id_, generation, sequence, parsed ? ParseStatus::Ok : result.status, std::move(result.message)});
    }
    if (dirty_ && !stopped_) {
      WorkerPool::shared().post([self = shared_from_this()] { self->step(); });
    } else {
      scheduled_ = false;
    }
  }

  bool isCurrentGeneration(uint64_t generation) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !stopped_ && generation == generation_;
  }

  mutable std::mutex slotMutex_;
  SessionSlotCache<FenceSlot> fenceCache_;
  SessionSlotCache<TagSlot> tagCache_;
  uint64_t tagStructureDocument_{0};
  uint64_t tagStructureBuilds_{0};
  std::atomic<uint64_t> sourceCaptureBytes_{0};
  std::atomic<uint64_t> tagBodyCopyBytes_{0};
  std::vector<TagSlotRange> tagStructure_;
  const std::string id_;
  // Used only by the job in progress; jobs for one session never overlap.
  IncrementalParser parser_;
  const SessionCallback callback_;
  mutable std::mutex mutex_;
  std::optional<std::string> replacement_;
  std::string pending_;
  size_t sourceBytes_{0};
  uint64_t generation_{0};
  uint64_t sequence_{0};
  uint64_t readyGeneration_{0};
  uint64_t readySequence_{0};
  DocumentHandle snapshot_;
  std::shared_ptr<const SharedList<std::string>::Prefix> snapshotSource_;
  std::shared_ptr<const TagSourceBodies> snapshotTagBodies_;
  std::shared_ptr<const std::vector<TagSlotRange>> snapshotTagStructure_;
  // Worker-only append lineage. A replacement starts a new immutable prefix.
  std::shared_ptr<const SharedList<std::string>::Prefix> tagSource_;
  std::unordered_map<uint64_t, std::shared_ptr<const std::string>> tagBodyRanges_;
  bool snapshotLive_{false};
  // The last document a view showed for this session. Not the snapshot: a view
  // may be showing a snapshot that has since been superseded.
  DocumentHandle shown_;
  std::vector<LayoutHint> layoutHints_;
  std::optional<remend::PreviewOptions> preview_;
  // Retain the configured options after the worker consumes an update, so
  // changing tags alone cannot reset the flags to their defaults.
  ParseOptions parse_;
  bool parsePending_{false};
  bool dirty_{false};
  bool scheduled_{false};
  bool finished_{false};
  bool stopped_{false};
};

std::mutex registryMutex;
std::unordered_map<std::string, std::shared_ptr<Session>> sessions;

std::shared_ptr<Session> findSession(const std::string &id) {
  std::lock_guard<std::mutex> lock(registryMutex);
  const auto found = sessions.find(id);
  return found == sessions.end() ? nullptr : found->second;
}

} // namespace

bool SessionRegistry::create(const std::string &id, ParseMode mode, SessionCallback callback) {
  if (id.empty() || !callback) {
    return false;
  }
  std::lock_guard<std::mutex> lock(registryMutex);
  if (sessions.count(id) != 0) {
    return false;
  }
  sessions.emplace(id, std::make_shared<Session>(id, mode, std::move(callback)));
  return true;
}

bool SessionRegistry::append(const std::string &id, const std::string &chunk, uint64_t generation,
                             uint64_t sequence) {
  const auto session = findSession(id);
  return session && session->append(chunk, generation, sequence);
}

bool SessionRegistry::reset(const std::string &id, const std::string &source, uint64_t generation) {
  const auto session = findSession(id);
  return session && session->reset(source, generation);
}

bool SessionRegistry::finish(const std::string &id, uint64_t generation, uint64_t sequence) {
  const auto session = findSession(id);
  return session && session->finish(generation, sequence);
}

void SessionRegistry::dispose(const std::string &id) {
  std::shared_ptr<Session> removed;
  {
    std::lock_guard<std::mutex> lock(registryMutex);
    const auto found = sessions.find(id);
    if (found != sessions.end()) {
      removed = std::move(found->second);
      sessions.erase(found);
    }
  }
  if (removed) {
    SessionTrace::disable(id);
    removed->stop();
  }
}

DocumentHandle SessionRegistry::snapshot(const std::string &id, uint64_t generation, uint64_t sequence) {
  const auto session = findSession(id);
  return session ? session->snapshot(generation, sequence) : nullptr;
}

PublishedSessionSnapshot SessionRegistry::publishedSnapshot(const std::string &id, uint64_t generation,
                                                            uint64_t sequence, bool includeSource) {
  const auto session = findSession(id);
  return session ? session->publishedSnapshot(generation, sequence, includeSource) : PublishedSessionSnapshot{};
}

SlotChanges<FenceSlot> SessionRegistry::fenceSlotChanges(const std::string &id, uint64_t generation,
    uint64_t sequence, const std::vector<std::string> &languages, uint64_t baseToken) {
  const auto session = findSession(id);
  return session ? session->fenceSlotChanges(generation, sequence, languages, baseToken) : SlotChanges<FenceSlot>{};
}

SlotChanges<TagSlot> SessionRegistry::tagSlotChanges(const std::string &id, uint64_t generation,
    uint64_t sequence, const std::vector<std::string> &names, uint64_t baseToken) {
  const auto session = findSession(id);
  return session ? session->tagSlotChanges(generation, sequence, names, baseToken) : SlotChanges<TagSlot>{};
}

SlotQueryWork SessionRegistry::slotWork(const std::string &id) {
  const auto session = findSession(id);
  return session ? session->slotWork() : SlotQueryWork{};
}

bool SessionRegistry::isFinished(const std::string &id) {
  const auto session = findSession(id);
  return session != nullptr && session->isFinished();
}

void SessionRegistry::noteLayout(const std::string &id, LayoutHint hint) {
  if (const auto session = findSession(id)) {
    session->noteLayout(hint);
  }
}

void SessionRegistry::noteShown(const std::string &id, const DocumentHandle &document) {
  if (const auto session = findSession(id)) {
    session->noteShown(document);
  }
}

DocumentHandle SessionRegistry::shown(const std::string &id) {
  const auto session = findSession(id);
  return session ? session->shown() : nullptr;
}

bool SessionRegistry::setPreview(const std::string &id, remend::PreviewOptions options) {
  const auto session = findSession(id);
  if (!session) {
    return false;
  }
  session->setPreview(options);
  return true;
}

bool SessionRegistry::setParse(const std::string &id, ParseOptions parse) {
  const auto session = findSession(id);
  if (!session) {
    return false;
  }
  session->setParse(std::move(parse));
  return true;
}

bool SessionRegistry::setParseTags(const std::string &id, TagPolicy tags) {
  const auto session = findSession(id);
  if (!session) {
    return false;
  }
  session->setParseTags(std::move(tags));
  return true;
}

} // namespace downstream
