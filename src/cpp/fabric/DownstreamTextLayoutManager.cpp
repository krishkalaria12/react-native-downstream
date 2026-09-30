#include "DownstreamTextLayoutManager.h"
#include "SessionTrace.h"

#include <algorithm>
#include <cmath>

namespace facebook::react {

namespace {

constexpr size_t kMeasurementCacheCapacity = 64;

} // namespace

Size DownstreamTextLayoutManager::measure(const downstream::DocumentHandle &document, Float fontSizeMultiplier,
                                          const LayoutConstraints &layoutConstraints, SurfaceId surfaceId,
                                          uint32_t chrome, uint32_t revision) const {
  const Float maximumWidth = layoutConstraints.maximumSize.width;
  Size size{0, 0};
  if (document) downstream::SessionTrace::recordDocument(document->id, "measure_start");

  if (document && !document->blocks.empty()) {
    bool cached = false;
    {
      std::lock_guard<std::mutex> lock(cacheMutex_);
      for (const auto &entry : cache_) {
        if (entry.documentId == document->id && entry.fontSizeMultiplier == fontSizeMultiplier &&
            entry.maximumWidth == maximumWidth && entry.chrome == chrome && entry.revision == revision) {
          size = entry.size;
          cached = true;
          break;
        }
      }
    }

    if (!cached) {
      size = measurePlatform(document, fontSizeMultiplier, maximumWidth, surfaceId, chrome);
      std::lock_guard<std::mutex> lock(cacheMutex_);
      if (cache_.size() >= kMeasurementCacheCapacity) {
        cache_.erase(cache_.begin());
      }
      cache_.push_back({document->id, fontSizeMultiplier, maximumWidth, chrome, revision, size});
    } else {
      downstream::SessionTrace::recordDocument(document->id, "measure_cache_hit");
    }
  }

  // Block content fills the available width, like a paragraph on the web.
  if (std::isfinite(maximumWidth)) {
    size.width = maximumWidth;
  }
  size.width = std::max(size.width, layoutConstraints.minimumSize.width);
  size.height = std::max(size.height, layoutConstraints.minimumSize.height);
  if (std::isfinite(layoutConstraints.maximumSize.height)) {
    size.height = std::min(size.height, layoutConstraints.maximumSize.height);
  }
  if (document) downstream::SessionTrace::recordDocument(document->id, "measure_end");
  return size;
}

} // namespace facebook::react
