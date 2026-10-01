#include "DownstreamTextLayoutManager.h"

#import "DSRenderedDocument.h"

#include "theme/Theme.h"

namespace facebook::react {

namespace {

/// The theme a layout is prepared and measured with.
///
/// A null handle means the default theme, which is what an app that passes
/// neither prop gets. Resolving it once here means every call below builds the
/// same rendering the view mounts, rather than one for a view that has a theme
/// and one for a view that does not.
const downstream::Theme::Handle &resolve(const downstream::Theme::Handle &theme) {
  return theme ? theme : downstream::Theme::defaultTheme();
}

} // namespace

// TextKit objects that are not attached to views are safe to use off the main
// thread, so measurement runs on the layout thread without blocking UIKit.
Size DownstreamTextLayoutManager::measurePlatform(const downstream::DocumentHandle &document,
                                                  Float fontSizeMultiplier, Float maximumWidth,
                                                  SurfaceId /*surfaceId*/, uint32_t chrome, bool codeBlockWrap,
                                                  const std::vector<uint32_t> &wrapToggles,
                                                  const downstream::Theme::Handle &theme) const {
  @autoreleasepool {
    // The view's own theme, so a themed measure and a themed draw are the same
    // numbers. Colors do not change metrics, so this is valid whether or not the
    // view turns them on; the view builds the rendering it draws.
    DSRenderedDocument *rendered = [DSRenderedDocument renderedDocumentFor:document
                                                                 fontScale:fontSizeMultiplier
                                                        syntaxHighlighting:YES
                                                                     theme:resolve(theme)];
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
                                                      syntaxHighlighting:YES
                                                                       theme:resolve(hint.theme)];
      [rendered sizeForWidth:hint.width
                     chrome:hint.chrome
              codeBlockWrap:hint.codeBlockWrap
                wrapToggles:hint.wrapToggles];
    }
  }
}

} // namespace facebook::react
