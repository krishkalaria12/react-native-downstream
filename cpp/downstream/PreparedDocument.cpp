#include "PreparedDocument.h"

#include "RenderPreparer.h"

#include <list>
#include <mutex>

namespace downstream {
namespace {

// Recent sources stay parsed, so a list that recycles message views does not
// parse and lay out the same message again. Bounded by count and source size.
constexpr size_t kCacheEntries = 64;
constexpr size_t kCacheSourceBytes = 4 * 1024 * 1024;

struct Cache {
  std::mutex mutex;
  // Most recently requested first.
  std::list<std::shared_ptr<PreparedDocument>> entries;
  size_t sourceBytes{0};
};

Cache &cache() {
  static auto *instance = new Cache();
  return *instance;
}

} // namespace

PreparedDocument::PreparedDocument(std::string source) : source_(std::move(source)) {}

void PreparedDocument::prepare() {
  result_ = parseMarkdown(source_);
  if (result_.status == ParseStatus::Ok) {
    RenderPreparer::prepare(result_.document, RenderPreparer::recentLayouts());
  }
}

std::shared_ptr<PreparedDocument> PreparedDocument::request(std::string_view source) {
  Cache &shared = cache();
  std::shared_ptr<PreparedDocument> prepared;
  {
    std::lock_guard<std::mutex> lock(shared.mutex);
    for (auto it = shared.entries.begin(); it != shared.entries.end(); ++it) {
      if ((*it)->source() == source) {
        prepared = *it;
        shared.entries.splice(shared.entries.begin(), shared.entries, it);
        return prepared;
      }
    }
    prepared = std::make_shared<PreparedDocument>(std::string(source));
    // A preparation nobody holds any more is skipped rather than run.
    prepared->task_ = std::make_shared<Task>([weak = std::weak_ptr<PreparedDocument>(prepared)] {
      if (const auto self = weak.lock()) {
        self->prepare();
      }
    });
    shared.entries.push_front(prepared);
    shared.sourceBytes += source.size();
    while (shared.entries.size() > 1 &&
           (shared.entries.size() > kCacheEntries || shared.sourceBytes > kCacheSourceBytes)) {
      shared.sourceBytes -= shared.entries.back()->source().size();
      shared.entries.pop_back();
    }
  }
  prepared->task_->schedule();
  return prepared;
}

void PreparedDocument::clearCache() {
  Cache &shared = cache();
  std::lock_guard<std::mutex> lock(shared.mutex);
  shared.entries.clear();
  shared.sourceBytes = 0;
}

} // namespace downstream
