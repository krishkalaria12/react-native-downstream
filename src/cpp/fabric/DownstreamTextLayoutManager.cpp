#include "DownstreamTextLayoutManager.h"
#include "SessionTrace.h"
#include "theme/ThemeLayout.h"

#include <algorithm>
#include <cmath>

namespace facebook::react {

namespace {

constexpr size_t kMeasurementCacheCapacity = 64;

} // namespace

uint64_t DownstreamTextLayoutManager::themeKey(const downstream::Theme::Handle &theme) {
  return theme ? theme->layoutKey() : 0;
}

DocumentMeasurement DownstreamTextLayoutManager::measure(const downstream::DocumentHandle &document,
                                                         Float fontSizeMultiplier,
                                                         const LayoutConstraints &layoutConstraints,
                                                         SurfaceId surfaceId, uint32_t chrome,
                                                         const downstream::CodeLayout &code,
                                                         const downstream::PlacedCaret &caret,
                                                         const downstream::ImageLayout &images,
                                                         const downstream::HostLayout &hosts,
                                                         const downstream::Theme::Handle &theme,
                                                         const downstream::TableLayout &tables) const {
  const Float maximumWidth = layoutConstraints.maximumSize.width;
  // The theme can cap the platform's font scale, and the cap is applied before
  // the scale reaches either the cache key or the platform, so a measurement and
  // the view that draws it are at the same size.
  const Float scale = static_cast<Float>(
      downstream::theme_layout::fontScale(theme ? *theme : *downstream::Theme::defaultTheme(),
                                          fontSizeMultiplier));
  const uint64_t themeLayoutKey = themeKey(theme);
  DocumentMeasurement measurement;
  if (document) downstream::SessionTrace::recordDocument(document->id, "measure_start");

  if (document && !document->blocks.empty()) {
    bool cached = false;
    {
      std::lock_guard<std::mutex> lock(cacheMutex_);
      for (const auto &entry : cache_) {
        if (entry.documentId == document->id && entry.fontSizeMultiplier == scale &&
            entry.maximumWidth == maximumWidth && entry.chrome == chrome && entry.code == code &&
            entry.caret == caret && entry.images == images && entry.hosts == hosts &&
            entry.tables == tables && entry.themeLayoutKey == themeLayoutKey) {
          measurement = entry.measurement;
          cached = true;
          break;
        }
      }
    }

    if (!cached) {
      measurement =
          measurePlatform(document, scale, maximumWidth, surfaceId, chrome, code, caret, images, hosts, theme,
                          tables);
      std::lock_guard<std::mutex> lock(cacheMutex_);
      if (cache_.size() >= kMeasurementCacheCapacity) {
        cache_.erase(cache_.begin());
      }
      cache_.push_back(
          {document->id, scale, maximumWidth, chrome, code, caret, images, hosts, tables, themeLayoutKey,
           measurement});
    } else {
      downstream::SessionTrace::recordDocument(document->id, "measure_cache_hit");
    }
  }

  // Block content fills the available width, like a paragraph on the web.
  if (std::isfinite(maximumWidth)) {
    measurement.size.width = maximumWidth;
  }
  measurement.size.width = std::max(measurement.size.width, layoutConstraints.minimumSize.width);
  measurement.size.height = std::max(measurement.size.height, layoutConstraints.minimumSize.height);
  if (std::isfinite(layoutConstraints.maximumSize.height)) {
    measurement.size.height = std::min(measurement.size.height, layoutConstraints.maximumSize.height);
  }
  if (document) downstream::SessionTrace::recordDocument(document->id, "measure_end");
  return measurement;
}

} // namespace facebook::react
