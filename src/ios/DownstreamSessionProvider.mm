#import <Foundation/Foundation.h>
#import <ReactCommon/RCTTurboModule.h>
#import <UIKit/UIKit.h>

#include "Clipboard.h"
#include "DownstreamSessionModule.h"
#include "DownstreamSessionFrameClock.h"


namespace {

/// `UIPasteboard` from C++.
///
/// `UIPasteboard` is main-thread-only for the general pasteboard, so the write
/// hops rather than blocking a caller's thread on a main-queue turn: the promise
/// resolves from the hop, which is what makes the copy land before the
/// confirmation's own dismissal animation finishes.
bool pasteOnMainThread(std::string_view text) {
  __block bool written = false;
  dispatch_async(dispatch_get_main_queue(), ^{
    @autoreleasepool {
      NSString *value = [[NSString alloc] initWithBytes:text.data()
                                                 length:text.size()
                                               encoding:NSUTF8StringEncoding];
      if (value != nil) {
        [UIPasteboard generalPasteboard].string = value;
        written = true;
      }
    }
  });
  return true;
}

} // namespace

/// Registered through codegenConfig.ios.modulesProvider. The module itself
/// is shared C++ in src/cpp/session.
@interface DownstreamSessionProvider : NSObject <RCTModuleProvider>
@end

@implementation DownstreamSessionProvider

+ (void)initialize {
  if (self == [DownstreamSessionProvider class]) {
    downstream::installClipboardWriter(&pasteOnMainThread);
    downstream::installSessionFrameClock();
  }
}

- (std::shared_ptr<facebook::react::TurboModule>)getTurboModule:
    (const facebook::react::ObjCTurboModule::InitParams &)params {
  return std::make_shared<facebook::react::DownstreamSessionModule>(params.jsInvoker);
}

@end
