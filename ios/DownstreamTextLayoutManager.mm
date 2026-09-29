#include "DownstreamTextLayoutManager.h"

#import "DSRenderedDocument.h"

namespace facebook::react {

// TextKit objects that are not attached to views are safe to use off the main
// thread, so measurement runs on the layout thread without blocking UIKit.
Size DownstreamTextLayoutManager::measurePlatform(const downstream::DocumentHandle &document,
                                                  Float fontSizeMultiplier, Float maximumWidth,
                                                  SurfaceId /*surfaceId*/) const {
  @autoreleasepool {
    DSRenderedDocument *rendered = [DSRenderedDocument renderedDocumentFor:document fontScale:fontSizeMultiplier];
    const CGSize size = [rendered sizeForWidth:maximumWidth];
    return {static_cast<Float>(size.width), static_cast<Float>(size.height)};
  }
}

void DownstreamTextLayoutManager::prepare(const downstream::DocumentHandle &document,
                                          const std::vector<downstream::LayoutHint> &hints) {
  @autoreleasepool {
    for (const auto &hint : hints) {
      DSRenderedDocument *rendered = [DSRenderedDocument renderedDocumentFor:document fontScale:hint.fontScale];
      [rendered sizeForWidth:hint.width];
    }
  }
}

} // namespace facebook::react
