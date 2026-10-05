#include "DownstreamTextLayoutManager.h"

#import "DSRenderedDocument.h"

#import <React/RCTImageLoaderProtocol.h>
#import <React/RCTImageLoaderWithAttributionProtocol.h>
#import <React/RCTUtils.h>
#import <react/utils/ManagedObjectWrapper.h>

#include "ImageLoaderRegistry.h"
#include "theme/Theme.h"

#include <memory>
#include <mutex>

namespace facebook::react {

namespace {

/// The container the image resolver reads, installed once by
/// `installPlatformServices` below.
std::shared_ptr<const facebook::react::ContextContainer> &DSImageContext() {
  static std::shared_ptr<const facebook::react::ContextContainer> container;
  return container;
}

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

/// The app's own image loader, or null.
///
/// Read exactly the way React Native's own `ImageManager` reads it: the context
/// container holds it under `"RCTImageLoader"` as a managed object, and
/// `unwrapManagedObject` is what gets the Objective-C pointer back. That is the
/// object React Native's own `<Image>` loads through, so the app's HTTP stack, its
/// cookies, its disk cache, and any custom URL handlers all apply to a picture a
/// model wrote. A separate fetcher would do none of that.
///
/// A function rather than a stored pointer because JavaScript asks on every image
/// load and the container outlives every view: a view that mounts after the bridge is
/// up still finds the loader.
static void *DSResolveReactImageLoader() {
  // Read through the accessor every time rather than caching into a `static` copy:
  // a static initialised from it would take whatever the value was on the first
  // call, which is before any container has been installed.
  const auto &container = DSImageContext();
  if (container == nullptr) {
    return nullptr;
  }
  // `at` throws when the key is absent, and the loader is only registered once the
  // bridge is up. `find` is the answer to that, so a view that mounts first draws
  // its placeholder rather than unwinding.
  auto loader = container->find<std::shared_ptr<void>>("RCTImageLoader");
  if (!loader.has_value() || !loader.value()) {
    return nullptr;
  }
  // A bridged cast to the registry's opaque handle. `__bridge` and not
  // `__bridge_retained`: the context container owns this loader and keeps it alive,
  // and the registry only borrows it for the duration of one load, so taking
  // ownership here would either leak or over-release. The engine never names an
  // Objective-C class, and the value comes back to this platform's own loader,
  // which is the only code that knows what it is.
  return (__bridge void *)unwrapManagedObject(loader.value());
}

void DownstreamTextLayoutManager::installPlatformServices(
    const facebook::react::ContextContainer &contextContainer) {
  // Referenced, not copied: a `ContextContainer` owns its instances behind its own
  // lock and offers no way to construct one from another. The layout manager holds
  // a shared pointer to the same container for the life of the process, and this
  // keeps one of its own because the manager is per bridge while the answer is per
  // process. The no-op deleter is the honest one: this code does not own the
  // container and must not free it.
  static std::mutex once;
  {
    std::lock_guard<std::mutex> lock(once);
    DSImageContext() = std::shared_ptr<const facebook::react::ContextContainer>(&contextContainer, [](const void *) {});
  }
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
                                                  const downstream::PlacedCaret &caret,
                                                  const downstream::ImageLayout &images,
                                                  const downstream::Theme::Handle &theme) const {
  @autoreleasepool {
    // The view's own theme, so a themed measure and a themed draw are the same
    // numbers. Colors do not change metrics, so this is valid whether or not the
    // view turns them on; the view builds the rendering it draws.
    // The caret is measured with everything else rather than added at draw time:
    // its placeholder is a real character in the measured text, so measuring
    // without it and drawing with it would put the last line in the wrong place.
    DSRenderedDocument *rendered = [DSRenderedDocument renderedDocumentFor:document
                                                                 fontScale:fontSizeMultiplier
                                                        syntaxHighlighting:YES
                                                                     theme:resolve(theme)
                                                                    caret:caret];
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
                                                                       theme:resolve(hint.theme)
                                                                      caret:hint.caret];
      [rendered sizeForWidth:hint.width
                       chrome:hint.chrome
                          code:hint.code
                        images:hint.images.state];
    }
  }
}

} // namespace facebook::react
