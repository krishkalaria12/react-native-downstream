#include "PreparedDocument.h"

#include "CodeBlocks.h"
#include "CodeHighlighter.h"
#include "RenderPreparer.h"

#include <algorithm>
#include <limits>
#include <list>
#include <mutex>

namespace downstream {
namespace {

// Recent sources stay parsed, so a list that recycles message views does not
// parse and lay out the same message again. Bounded by count and source size.
constexpr size_t kCacheEntries = 64;
constexpr size_t kCacheSourceBytes = 4 * 1024 * 1024;
// Layouts one source is kept prepared for. A view shows at one or two widths, so
// this is a ceiling rather than a working set size.
constexpr size_t kNotedHints = 4;

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

void PreparedDocument::noteHint(const LayoutHint &hint) {
  std::lock_guard<std::mutex> lock(hintMutex_);
  const auto found = std::find(hints_.begin(), hints_.end(), hint);
  if (found != hints_.end()) {
    hints_.erase(found);
  }
  hints_.insert(hints_.begin(), hint);
  // A view shows at one or two widths, so a handful is enough and the list
  // cannot grow with the number of views that happen to show this source.
  if (hints_.size() > kNotedHints) {
    hints_.resize(kNotedHints);
  }
}

std::vector<LayoutHint> PreparedDocument::hintsForPrepare() const {
  const std::vector<LayoutHint> recent = RenderPreparer::recentLayouts();
  std::lock_guard<std::mutex> lock(hintMutex_);
  std::vector<LayoutHint> hints;
  // This source's own hints come first, so a theme asked for here is prepared
  // even when the recent layouts are full of another view's.
  for (const LayoutHint &noted : hints_) {
    if (noted.width > 0) {
      hints.push_back(noted);
      continue;
    }
    // A hint with no width yet is a view that has not been measured, which is
    // every view at the moment its source is requested. It still knows its
    // theme, so it is expanded against the widths views use now rather than
    // dropped: otherwise the first view in a commit to show a source would be
    // prepared for whichever theme the last measured view used.
    for (const LayoutHint &at : recent) {
      LayoutHint expanded = noted;
      expanded.width = at.width;
      expanded.fontScale = at.fontScale;
      expanded.chrome = at.chrome;
      expanded.wrapToggles = at.wrapToggles;
      expanded.codeBlockWrap = at.codeBlockWrap;
      if (std::find(hints.begin(), hints.end(), expanded) == hints.end()) {
        hints.push_back(std::move(expanded));
      }
    }
  }
  for (const LayoutHint &at : recent) {
    if (std::find(hints.begin(), hints.end(), at) == hints.end()) {
      hints.push_back(at);
    }
  }
  return hints;
}

void PreparedDocument::prepare() {
  result_ = parseMarkdown(source_);
  if (result_.status == ParseStatus::Ok) {
    // Highlighting runs here, on the same worker job and so inside the
    // synchronous-layout budget, so the platform text work that follows sees
    // finished tokens.
    result_.document = highlightSettledCodeBlocks(lexillaHighlighter(), result_.document, kHighlightBudgetMs, nullptr);
    // A controlled source is complete the moment it is parsed and this result
    // is cached, so every block settles: a later snapshot would only come from
    // a different source, and this one will not be parsed again.
    const auto count = static_cast<uint32_t>(
        std::min<size_t>(result_.document->blocks.size(), std::numeric_limits<uint32_t>::max()));
    result_.document = withSettledBlocks(result_.document, count);
    // The document itself carries no theme, so one parse serves every theme and
    // only the platform rendering is per theme.
    RenderPreparer::prepare(result_.document, hintsForPrepare());
  }
}

std::shared_ptr<PreparedDocument> PreparedDocument::request(std::string_view source, const LayoutHint &hint) {
  Cache &shared = cache();
  std::shared_ptr<PreparedDocument> prepared;
  {
    std::lock_guard<std::mutex> lock(shared.mutex);
    for (auto it = shared.entries.begin(); it != shared.entries.end(); ++it) {
      if ((*it)->source() == source) {
        prepared = *it;
        shared.entries.splice(shared.entries.begin(), shared.entries, it);
        break;
      }
    }
    if (prepared == nullptr) {
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
  }
  // Noted after the entry is in the cache and outside the cache lock, so a
  // second view showing the same source adds its own theme rather than racing
  // for the first one's.
  prepared->noteHint(hint);
  if (!prepared->task_->done()) {
    prepared->task_->schedule();
  }
  return prepared;
}

void PreparedDocument::clearCache() {
  Cache &shared = cache();
  std::lock_guard<std::mutex> lock(shared.mutex);
  shared.entries.clear();
  shared.sourceBytes = 0;
}

} // namespace downstream
