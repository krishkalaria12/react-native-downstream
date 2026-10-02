#import "DSImageLoader.h"
#import <React/RCTUtils.h>

#include <cstdio>
#include <cstdlib>

// Keep production rendering and layout linked. Network transport and React
// presentation are outside this executable's fixtures; accidental use fails
// the process instead of silently pretending an image loaded or a save worked.
[[noreturn]] static void unavailable(const char *service) {
  fprintf(stderr, "FAIL: native TextKit harness requested unavailable %s\n", service);
  exit(1);
}

@implementation DSImageLoader
+ (instancetype)sharedLoaderWithReactLoader:(RCTImageLoader *)reactLoader {
  unavailable("React image loader");
}
- (NSUInteger)inFlightCount {
  unavailable("React image loader");
}
- (DSImageLoad *)loadURL:(NSURL *)url
                  size:(CGSize)size
                 scale:(CGFloat)scale
              callback:(void (^)(DSImageLoad *, DSImageLoadResult *))completion {
  unavailable("React image transport");
}
- (void)cancelLoad:(DSImageLoad *)load {
  unavailable("React image transport");
}
@end

void DSDownloadImageBytes(NSURL *url, void (^completion)(NSData *, NSString *)) {
  unavailable("image download transport");
}

UIViewController *RCTPresentedViewController(void) {
  unavailable("React view controller presentation");
}
