#pragma once

#include "CodeLayout.h"
#include "Document.h"
#include "HostLayout.h"
#include "ImageLayout.h"
#include "RenderPreparer.h"
#include "theme/Theme.h"

#include <react/renderer/core/LayoutConstraints.h>
#include <react/renderer/core/ReactPrimitives.h>
#include <react/renderer/graphics/Rect.h>
#include <react/utils/ContextContainer.h>

#include <memory>
#include <mutex>
#include <vector>

namespace facebook::react {

/// One hosted block's rectangle, in the document view's coordinates.
struct HostRect {
  uint32_t ordinal{0};
  Rect rect;
};

/// What one measurement produced: the document's size, and where each React
/// child that replaced a fence goes.
///
/// The rectangles come back from the same call that produced the size, so a
/// frame that draws at a position has that position from the measurement that
/// gave the document its height, rather than from a second pass that could
/// disagree about the column.
struct DocumentMeasurement {
  Size size{0, 0};
  std::vector<HostRect> slots;

  /// The rectangle for `ordinal`, or nullptr when this document hosts no
  /// child there, which is what a stale slot from a lagging render gets.
  const Rect *slotFor(uint32_t ordinal) const {
    for (const auto &slot : slots) {
      if (slot.ordinal == ordinal) {
        return &slot.rect;
      }
    }
    return nullptr;
  }
};

/// Measures a document with the platform text system. Implemented per
/// platform: TextKit on iOS (src/ios/DownstreamTextLayoutManager.mm) and
/// StaticLayout through the view manager on Android
/// (src/android/src/main/jni/DownstreamTextLayoutManager.cpp).
class DownstreamTextLayoutManager final {
public:
  explicit DownstreamTextLayoutManager(std::shared_ptr<const ContextContainer> contextContainer)
      : contextContainer_(std::move(contextContainer)) {
    downstream::RenderPreparer::install(&DownstreamTextLayoutManager::prepare);
    // The container is the only place the platform's own image loader lives, and
    // this is the first code of ours that runs with it in hand, so the install
    // happens here rather than in a view that may not exist yet.
    installPlatformServices(*contextContainer_);
  }

  /// Hands the engine the platform services that need the context container.
  /// Implemented per platform next to `measurePlatform`.
  static void installPlatformServices(const ContextContainer &contextContainer);

  /// Builds a document's platform rendering and measures it at each hint on
  /// the calling worker thread, filling the caches measurePlatform and the
  /// view read. Implemented per platform next to measurePlatform.
  static void prepare(const downstream::DocumentHandle &document, const std::vector<downstream::LayoutHint> &hints);

  /// Returns the content size for the document at the given width. Results
  /// are cached by document id, font scale, width, chrome, the code layout,
  /// the host layout and the image layout, and the theme's *layout* key; a new
  /// source, font scale, width, chrome, layout, or size-changing theme value
  /// is always measured again.
  ///
  /// The code layout is part of the key and part of the call because a document
  /// can hold wrapped and unwrapped code at once, capped and uncapped code at
  /// once, and code with and without a gutter at once, and a block's height
  /// depends on which. The platform decides each block through
  /// `downstream::resolveCodeBlock`, the same function the view draws with.
  ///
  /// The image layout is in the key for the same reason and the same shape: a
  /// document can hold a loaded picture and a placeholder at once, and one
  /// image's height changes when its load finishes. The platform decides each
  /// image through `downstream::imageBox`, so a measured picture and a drawn
  /// one cannot disagree about how tall it is.
  ///
  /// The host layout is in the key for the same reason again: a hosted fence
  /// is as tall as the React child that replaced it, and a document can hold a
  /// chart and a code card at once. An empty layout changes nothing, so a view
  /// with no renderers measures exactly as it did before this existed.
  ///
  /// The theme keys on `themeLayoutKey` and not on `id`, so a color-only change
  /// reuses every measured size and Yoga sees nothing at all: the drawing
  /// objects are rebuilt, but no measurement is.
  DocumentMeasurement measure(const downstream::DocumentHandle &document, Float fontSizeMultiplier,
                              const LayoutConstraints &layoutConstraints, SurfaceId surfaceId, uint32_t chrome,
                              const downstream::CodeLayout &code, const downstream::ImageLayout &images,
                              const downstream::HostLayout &hosts, const downstream::Theme::Handle &theme) const;

  /// The cached measurement for exactly the layout `measure` would use, so
  /// `layout()` can place the children that measurement sized without
  /// measuring the document a second time.
  ///
  /// Null when the cache has no such entry, which is the case before the first
  /// measurement of this revision; a caller that gets null places no child,
  /// which is the same answer as a view with no renderers.
  DocumentMeasurement cachedMeasurement(const downstream::DocumentHandle &document, Float fontSizeMultiplier,
                                        Float maximumWidth, uint32_t chrome, const downstream::CodeLayout &code,
                                        const downstream::ImageLayout &images,
                                        const downstream::HostLayout &hosts,
                                        const downstream::Theme::Handle &theme) const;

private:
  DocumentMeasurement measurePlatform(const downstream::DocumentHandle &document, Float fontSizeMultiplier,
                                      Float maximumWidth, SurfaceId surfaceId, uint32_t chrome,
                                      const downstream::CodeLayout &code, const downstream::ImageLayout &images,
                                      const downstream::HostLayout &hosts,
                                      const downstream::Theme::Handle &theme) const;

  /// The theme's id, or 0 for the default theme, which is its id too.
  static uint64_t themeKey(const downstream::Theme::Handle &theme);

  struct CacheEntry {
    uint64_t documentId;
    Float fontSizeMultiplier;
    Float maximumWidth;
    uint32_t chrome;
    downstream::CodeLayout code;
    downstream::ImageLayout images;
    downstream::HostLayout hosts;
    /// The theme's layout key, which is 0 for the default theme.
    uint64_t themeLayoutKey;
    DocumentMeasurement measurement;
  };

  std::shared_ptr<const ContextContainer> contextContainer_;
  mutable std::mutex cacheMutex_;
  mutable std::vector<CacheEntry> cache_;
};

} // namespace facebook::react