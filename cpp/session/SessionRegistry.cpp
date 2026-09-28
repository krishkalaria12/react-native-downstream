#include "SessionRegistry.h"

#include "MarkdownParser.h"
#include "Unicode.h"

#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>

namespace downstream {
namespace {

class Session {
public:
  Session(std::string id, ParseMode mode, SnapshotCallback callback)
      : id_(std::move(id)), parser_(mode), callback_(std::move(callback)), worker_([this] { run(); }) {}

  ~Session() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopped_ = true;
      dirty_ = false;
    }
    changed_.notify_one();
    if (worker_.joinable()) {
      worker_.join();
    }
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
        chunk.size() > 64 * 1024 || source_.size() + chunk.size() > kMaxSourceBytes) {
      return false;
    }
    source_.append(chunk);
    sequence_ = sequence;
    dirty_ = true;
    changed_.notify_one();
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
    source_ = source;
    generation_ = generation;
    sequence_ = 0;
    finished_ = false;
    appendOnly_ = false;
    dirty_ = true;
    snapshot_.reset();
    readyGeneration_ = 0;
    readySequence_ = 0;
    changed_.notify_one();
    return true;
  }

  bool finish(uint64_t generation, uint64_t sequence) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_ || generation != generation_ || sequence != sequence_) {
      return false;
    }
    if (!finished_) {
      finished_ = true;
      dirty_ = true;
      changed_.notify_one();
    }
    return true;
  }

  DocumentHandle snapshot(uint64_t generation, uint64_t sequence) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return generation == readyGeneration_ && sequence == readySequence_ && generation == generation_
               ? snapshot_
               : nullptr;
  }

private:
  void run() {
    uint64_t parserGeneration = 0;
    for (;;) {
      std::string source;
      uint64_t generation;
      uint64_t sequence;
      bool appendOnly;
      bool finished;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        changed_.wait(lock, [this] { return stopped_ || dirty_; });
        if (stopped_) {
          return;
        }
        source = source_;
        generation = generation_;
        sequence = sequence_;
        appendOnly = appendOnly_;
        finished = finished_;
        dirty_ = false;
        appendOnly_ = true;
      }

      if (generation != parserGeneration) {
        parser_.reset();
        parserGeneration = generation;
        appendOnly = false;
      }
      auto result = parser_.update(source, appendOnly, finished);
      if (result.status != ParseStatus::Ok || !result.document) {
        continue;
      }

      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_ || generation != generation_ || sequence != sequence_ || finished != finished_) {
          continue;
        }
        snapshot_ = std::move(result.document);
        readyGeneration_ = generation;
        readySequence_ = sequence;
      }
      callback_(id_, generation, sequence);
    }
  }

  const std::string id_;
  IncrementalParser parser_;
  const SnapshotCallback callback_;
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::thread worker_;
  std::string source_;
  uint64_t generation_{0};
  uint64_t sequence_{0};
  uint64_t readyGeneration_{0};
  uint64_t readySequence_{0};
  DocumentHandle snapshot_;
  bool dirty_{false};
  bool appendOnly_{true};
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

bool SessionRegistry::create(const std::string &id, ParseMode mode, SnapshotCallback callback) {
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
  // Joining the worker outside the registry lock lets other sessions proceed.
  removed.reset();
}

DocumentHandle SessionRegistry::snapshot(const std::string &id, uint64_t generation, uint64_t sequence) {
  const auto session = findSession(id);
  return session ? session->snapshot(generation, sequence) : nullptr;
}

} // namespace downstream
