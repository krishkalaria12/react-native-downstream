#include "PreparedDocument.h"

#include "CodeBlocks.h"
#include "CodeHighlighter.h"
#include "MathBlocks.h"
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

/**
 * A prepared document's key.
 *
 * The parse options are part of it, not an afterthought: two views showing one
 * string with different math options must not share a parse, because one of
 * them has formulas in it and the other does not. Equal sources with equal
 * options share a result, so the common case is unchanged.
 */
bool sameRequest(std::string_view source, const MathOptions &math, const PreparedDocument &prepared) {
  return prepared.source() == source && prepared.math() == math;
}

} // namespace

std::shared_ptr<const PreparedDocument> PreparedDocument::completed(std::string_view source, MathOptions math) {
  Cache &shared = cache();
  std::lock_guard<std::mutex> lock(shared.mutex);
  for (const auto &entry : shared.entries) {
    if (sameRequest(source, math, *entry) && entry->done()) {
      return entry;
    }
  }
  return nullptr;
}

DocumentHandle PreparedDocument::blocksFor(std::string_view source, MathOptions math) {
  const auto prepared = completed(source, math);
  if (!prepared) {
    return nullptr;
  }
  const ParseResult &result = prepared->result();
  return result.status == ParseStatus::Ok ? result.document : nullptr;
}

PreparedDocument::PreparedDocument(std::string source, MathOptions math)
    : source_(std::move(source)), math_(math) {}

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
  if (hints_.empty()) {
    return recent;
  }
  std::vector<LayoutHint> hints;
  // Prepare this source's own themes and controls. Recent layouts supply only
  // geometry for a view that has not yet been measured.
  for (const LayoutHint &noted : hints_) {
    if (noted.width > 0) {
      if (std::find(hints.begin(), hints.end(), noted) == hints.end()) {
        hints.push_back(noted);
      }
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
      if (std::find(hints.begin(), hints.end(), expanded) == hints.end()) {
        hints.push_back(std::move(expanded));
      }
    }
  }
  return hints;
}

void PreparedDocument::prepare() {
  result_ = parseMarkdown(source_, math_);
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
    // Math runs after settling, because a formula that fails in a settled block
    // has failed for good: its error is recorded, it turns the error color, and
    // the view reports it. Laid out before settling, every failure would read as
    // a formula that is still arriving. It is cheap: a document with no formula
    // returns the same handle and never calls the engine.
    result_.document = layoutMath(result_.document);
    // The document itself carries no theme, so one parse serves every theme and
    // only the platform rendering is per theme.
    RenderPreparer::prepare(result_.document, hintsForPrepare());
  }
}

std::shared_ptr<PreparedDocument> PreparedDocument::request(std::string_view source, const LayoutHint &hint,
                                                            MathOptions math) {
  Cache &shared = cache();
  std::shared_ptr<PreparedDocument> prepared;
  {
    std::lock_guard<std::mutex> lock(shared.mutex);
    for (auto it = shared.entries.begin(); it != shared.entries.end(); ++it) {
      if (sameRequest(source, math, **it)) {
        prepared = *it;
        shared.entries.splice(shared.entries.begin(), shared.entries, it);
        break;
      }
    }
    if (prepared == nullptr) {
      prepared = std::make_shared<PreparedDocument>(std::string(source), math);
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
