#include "RenderPreparer.h"

#include <algorithm>
#include <atomic>
#include <mutex>

namespace downstream {
namespace {

// Chat layouts measure most messages at one or two widths. Each hint costs a
// text layout per document, so only the latest few are prepared.
constexpr size_t kMaxHints = 2;

std::atomic<RenderPreparer::Prepare> installed{nullptr};
std::mutex hintMutex;
std::vector<LayoutHint> hints;

} // namespace

void RenderPreparer::install(Prepare prepare) {
  installed.store(prepare, std::memory_order_release);
}

void RenderPreparer::prepare(const DocumentHandle &document, const std::vector<LayoutHint> &layoutHints) {
  const Prepare prepare = installed.load(std::memory_order_acquire);
  if (prepare != nullptr && document && !document->blocks.empty() && !layoutHints.empty()) {
    prepare(document, layoutHints);
  }
}

void RenderPreparer::noteLayout(LayoutHint hint) {
  std::lock_guard<std::mutex> lock(hintMutex);
  if (!hints.empty() && hints.front() == hint) {
    return;
  }
  hints.erase(std::remove(hints.begin(), hints.end(), hint), hints.end());
  hints.insert(hints.begin(), hint);
  if (hints.size() > kMaxHints) {
    hints.resize(kMaxHints);
  }
}

std::vector<LayoutHint> RenderPreparer::recentLayouts() {
  std::lock_guard<std::mutex> lock(hintMutex);
  return hints;
}

} // namespace downstream
