#import <Foundation/Foundation.h>
#import <ReactCommon/RCTTurboModule.h>

#include "DownstreamSessionModule.h"

/// Registered through codegenConfig.ios.modulesProvider. The module itself
/// is shared C++ in src/cpp/session.
@interface DownstreamSessionProvider : NSObject <RCTModuleProvider>
@end

@implementation DownstreamSessionProvider

- (std::shared_ptr<facebook::react::TurboModule>)getTurboModule:
    (const facebook::react::ObjCTurboModule::InitParams &)params {
  return std::make_shared<facebook::react::DownstreamSessionModule>(params.jsInvoker);
}

@end
