#pragma once

#include "Document.h"

#include <react/renderer/core/LayoutConstraints.h>
#include <react/renderer/core/ReactPrimitives.h>
#include <react/utils/ContextContainer.h>

#include <memory>
#include <mutex>
#include <vector>

namespace facebook::react {

/// Measures a document with the platform text system. Implemented per
/// platform: TextKit on iOS (ios/DownstreamTextLayoutManager.mm) and
/// StaticLayout through the view manager on Android
/// (android/src/main/jni/DownstreamTextLayoutManager.cpp).
class DownstreamTextLayoutManager final {
public:
  explicit DownstreamTextLayoutManager(std::shared_ptr<const ContextContainer> contextContainer)
      : contextContainer_(std::move(contextContainer)) {}

  /// Returns the content size for the document at the given width. Results
  /// are cached by document id, font scale, and width; a new source, font
  /// scale, or width is always measured again.
  Size measure(const downstream::DocumentHandle &document, Float fontSizeMultiplier,
               const LayoutConstraints &layoutConstraints, SurfaceId surfaceId) const;

private:
  Size measurePlatform(const downstream::DocumentHandle &document, Float fontSizeMultiplier, Float maximumWidth,
                       SurfaceId surfaceId) const;

  struct CacheEntry {
    uint64_t documentId;
    Float fontSizeMultiplier;
    Float maximumWidth;
    Size size;
  };

  std::shared_ptr<const ContextContainer> contextContainer_;
  mutable std::mutex cacheMutex_;
  mutable std::vector<CacheEntry> cache_;
};

} // namespace facebook::react
