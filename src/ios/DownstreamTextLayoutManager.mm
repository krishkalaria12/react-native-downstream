#include "DownstreamTextLayoutManager.h"

#import "DSRenderedDocument.h"

#import <React/RCTImageLoader.h>
#import <React/RCTUtils.h>

#include "ImageLoaderRegistry.h"
#include "theme/Theme.h"

#include <memory>
#include <mutex>

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

/// Hands the engine the platform's own image loader, once.
///
/// The container is captured by the layout manager, which React Native builds
/// before any view, so the resolver below is a function rather than a stored
/// pointer: the container outlives every view, and a view that mounts after the
/// bridge is up still finds the loader. This is the same object React Native's
/// own `<Image>` reads, under the same key, so the app's HTTP stack, its cookies,
/// its disk cache, and any custom URL handlers all apply to a picture a model
/// wrote.
static RCTImageLoader *DSResolveReactImageLoader() {
  static std::shared_ptr<const facebook::react::ContextContainer> container;
  if (container == nullptr) {
    return nil;
  }
  RCTImageLoader *loader = container->getObject<RCTImageLoader>("RCTImageLoader");
  return loader != nullptr ? [loader weakVectorSingleton] : nil;
}

void DownstreamTextLayoutManager::installPlatformServices(
    const facebook::react::ContextContainer &contextContainer) {
  // The container is captured for the life of the process, so the resolver reads
  // it rather than being handed a pointer that could dangle. It is captured
  // rather than the layout manager's own copy because the manager is per bridge
  // and a second bridge should find the loader it brought.
  static std::shared_ptr<const facebook::react::ContextContainer> held;
  static std::mutex once;
  {
    std::lock_guard<std::mutex> lock(once);
    held = std::make_shared<const facebook::react::ContextContainer>(contextContainer);
  }
  (void)held;
  // The last install wins, so a bridge that arrives after an early view still
  // replaces the null a first bridge would have left.
  downstream::ImageLoaderRegistry::install(&DSResolveReactImageLoader);
}

// TextKit objects that are not attached to views are safe to use off the main
// thread, so measurement runs on the layout thread without blocking UIKit.
Size DownstreamTextLayoutManager::measurePlatform(const downstream::DocumentHandle &document,
                                                  Float fontSizeMultiplier, Float maximumWidth,
                                                  SurfaceId /*surfaceId*/, uint32_t chrome,
                                                  const downstream::CodeLayout &code,
                                                  const downstream::ImageLayout &images,
                                                  const downstream::Theme::Handle &theme) const {
  @autoreleasepool {
    // The view's own theme, so a themed measure and a themed draw are the same
    // numbers. Colors do not change metrics, so this is valid whether or not the
    // view turns them on; the view builds the rendering it draws.
    DSRenderedDocument *rendered = [DSRenderedDocument renderedDocumentFor:document
                                                                 fontScale:fontSizeMultiplier
                                                        syntaxHighlighting:YES
                                                                     theme:resolve(theme)];
    // The whole code layout comes along, because every input that changes a
    // card's height is in it: wrapping, the cap, the gutter, and the reader's
    // per-block choices. A document can hold every combination at once. The image
    // answers come along for the same reason: a picture's height is its state,
    // and a document can hold a loaded image and a placeholder at once.
    const CGSize size = [rendered sizeForWidth:maximumWidth
                                         chrome:chrome
                                            code:code
                                          images:images.state];
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
                          code:hint.code
                        images:hint.images.state];
    }
  }
}

} // namespace facebook::react
