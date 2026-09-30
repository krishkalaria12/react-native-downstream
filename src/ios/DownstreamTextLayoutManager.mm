#include "DownstreamTextLayoutManager.h"

#import "DSRenderedDocument.h"

namespace facebook::react {

// TextKit objects that are not attached to views are safe to use off the main
// thread, so measurement runs on the layout thread without blocking UIKit.
Size DownstreamTextLayoutManager::measurePlatform(const downstream::DocumentHandle &document,
                                                  Float fontSizeMultiplier, Float maximumWidth,
                                                  SurfaceId /*surfaceId*/, uint32_t chrome, bool codeBlockWrap,
                                                  const std::vector<uint32_t> &wrapToggles) const {
  @autoreleasepool {
    // Colors do not change metrics, so a measurement is valid whether or not
    // the view turns them on; the view builds the rendering it draws.
    DSRenderedDocument *rendered = [DSRenderedDocument renderedDocumentFor:document
                                                                 fontScale:fontSizeMultiplier
                                                        syntaxHighlighting:YES];
    // The wrap choices come along, because a code block's height depends on
    // whether it wraps and a document can hold both kinds at once.
    const CGSize size = [rendered sizeForWidth:maximumWidth
                                        chrome:chrome
                                 codeBlockWrap:codeBlockWrap
                                   wrapToggles:wrapToggles];
    return {static_cast<Float>(size.width), static_cast<Float>(size.height)};
  }
}

void DownstreamTextLayoutManager::prepare(const downstream::DocumentHandle &document,
                                          const std::vector<downstream::LayoutHint> &hints) {
  @autoreleasepool {
    for (const auto &hint : hints) {
      DSRenderedDocument *rendered = [DSRenderedDocument renderedDocumentFor:document
                                                                   fontScale:hint.fontScale
                                                      syntaxHighlighting:YES];
      [rendered sizeForWidth:hint.width
                     chrome:hint.chrome
              codeBlockWrap:hint.codeBlockWrap
                wrapToggles:hint.wrapToggles];
    }
  }
}

} // namespace facebook::react
