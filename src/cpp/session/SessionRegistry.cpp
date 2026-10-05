#include "SessionRegistry.h"

#include "MarkdownParser.h"
#include "RenderPreparer.h"
#include "SessionTrace.h"
#include "Unicode.h"
#include "WorkerPool.h"

#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
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

  DocumentHandle snapshot(uint64_t generation, uint64_t sequence) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return generation == readyGeneration_ && sequence == readySequence_ && generation == generation_
               ? snapshot_
               : nullptr;
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
    if (!finished_) {
      markDirty();
    }
  }

  void noteLayout(LayoutHint hint) {
    std::lock_guard<std::mutex> lock(mutex_);
    layout_ = hint;
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
      parse.swap(parse_);
      generation = generation_;
      sequence = sequence_;
      finished = finished_;
      dirty_ = false;
      if (layout_) {
        hints.push_back(*layout_);
      }
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
    }
    parser_.append(pending);
    auto result = parser_.parse(finished);
    SessionTrace::record(id_, generation, sequence, "parse_end");
    const bool parsed = result.status == ParseStatus::Ok && result.document;

    if (parsed && isCurrentGeneration(generation)) {
      // The view has not measured yet, so the widths other views use are the
      // best guess for this one.
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
        SessionTrace::bindDocument(snapshot_, id_, generation, sequence);
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
  std::optional<LayoutHint> layout_;
  std::optional<remend::PreviewOptions> preview_;
  std::optional<ParseOptions> parse_;
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

void SessionRegistry::noteLayout(const std::string &id, LayoutHint hint) {
  if (const auto session = findSession(id)) {
    session->noteLayout(hint);
  }
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
  session->setParse(parse);
  return true;
}

} // namespace downstream
