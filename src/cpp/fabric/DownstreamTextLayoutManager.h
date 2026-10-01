#pragma once

#include "Document.h"
#include "RenderPreparer.h"
#include "theme/Theme.h"

#include <react/renderer/core/LayoutConstraints.h>
#include <react/renderer/core/ReactPrimitives.h>
#include <react/utils/ContextContainer.h>

#include <memory>
#include <mutex>
#include <vector>

namespace facebook::react {

/// Measures a document with the platform text system. Implemented per
/// platform: TextKit on iOS (src/ios/DownstreamTextLayoutManager.mm) and
/// StaticLayout through the view manager on Android
/// (src/android/src/main/jni/DownstreamTextLayoutManager.cpp).
class DownstreamTextLayoutManager final {
public:
  explicit DownstreamTextLayoutManager(std::shared_ptr<const ContextContainer> contextContainer)
      : contextContainer_(std::move(contextContainer)) {
    downstream::RenderPreparer::install(&DownstreamTextLayoutManager::prepare);
  }

  /// Builds a document's platform rendering and measures it at each hint on
  /// the calling worker thread, filling the caches measurePlatform and the
  /// view read. Implemented per platform next to measurePlatform.
  static void prepare(const downstream::DocumentHandle &document, const std::vector<downstream::LayoutHint> &hints);

  /// Returns the content size for the document at the given width. Results
  /// are cached by document id, font scale, width, chrome, the reader's wrap
  /// choices, and the theme's *layout* key; a new source, font scale, width,
  /// chrome, set of choices, or size-changing theme value is always measured
  /// again.
  ///
  /// The wrap choices are part of the key and part of the call because a
  /// document can hold wrapped and unwrapped code at once, and a block's height
  /// depends on which it is. The platform decides each block through
  /// `downstream::codeWraps`, the same function the view draws with.
  ///
  /// The theme keys on `layoutKey` and not on `id`, so a color-only change
  /// reuses every measured size and Yoga sees nothing at all: the drawing
  /// objects are rebuilt, but no measurement is.
  Size measure(const downstream::DocumentHandle &document, Float fontSizeMultiplier,
               const LayoutConstraints &layoutConstraints, SurfaceId surfaceId, uint32_t chrome, bool codeBlockWrap,
               const std::vector<uint32_t> &wrapToggles, const downstream::Theme::Handle &theme) const;

private:
  Size measurePlatform(const downstream::DocumentHandle &document, Float fontSizeMultiplier, Float maximumWidth,
                       SurfaceId surfaceId, uint32_t chrome, bool codeBlockWrap,
                       const std::vector<uint32_t> &wrapToggles, const downstream::Theme::Handle &theme) const;

  struct CacheEntry {
    uint64_t documentId;
    Float fontSizeMultiplier;
    Float maximumWidth;
    uint32_t chrome;
    bool codeBlockWrap;
    std::vector<uint32_t> wrapToggles;
    /// The theme's layout key, which is 0 for the default theme.
    uint64_t themeLayoutKey;
    Size size;
  };

  std::shared_ptr<const ContextContainer> contextContainer_;
  mutable std::mutex cacheMutex_;
  mutable std::vector<CacheEntry> cache_;
};

} // namespace facebook::react