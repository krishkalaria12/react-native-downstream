// Installs RaTeX into Downstream when the app loads.
//
// The whole of this package's iOS code. RaTeX's archive is linked into the app by
// the pod, and `+load` hands its three C functions and the KaTeX font bundle to
// Downstream before any view exists, so `mathAvailable()` is true on the first
// render and the first formula is laid out without a second step.

#import <Foundation/Foundation.h>

// The XCFramework puts its Headers directory on this pod's search path.
#import "ratex.h"

// Downstream's install entry point. Declared here rather than imported, because
// Downstream's headers are private to its pod; a C function taking addresses is
// the one surface another pod can call. Defined in Downstream's DSMathFonts.mm.
extern "C" void DownstreamInstallMathEngine(const void *parseAndLayout, const void *freeDisplayList,
                                            const void *lastError, const char *fontDirectory);

@interface DSMathInstaller : NSObject
@end

@implementation DSMathInstaller

+ (void)load {
  // The font bundle sits next to this class, inside the app or inside a
  // framework when the app links pods dynamically.
  NSBundle *home = [NSBundle bundleForClass:self];
  NSString *fonts = [home pathForResource:@"DownstreamMathFonts" ofType:@"bundle"];
  DownstreamInstallMathEngine(reinterpret_cast<const void *>(&ratex_parse_and_layout),
                              reinterpret_cast<const void *>(&ratex_free_display_list),
                              reinterpret_cast<const void *>(&ratex_get_last_error),
                              fonts != nil ? fonts.fileSystemRepresentation : "");
}

@end
