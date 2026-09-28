#import <Foundation/Foundation.h>

#import <DownstreamSpec/DownstreamSpec.h>
#import <React/RCTBridgeModule.h>

#include "SessionRegistry.h"

#include <cmath>
#include <cstdint>
#include <exception>
#include <string>

using namespace facebook::react;

namespace {

std::string utf8(NSString *value) {
  const char *bytes = value.UTF8String;
  return bytes == nullptr ? std::string() : std::string(bytes);
}

bool validCounter(double value) {
  return std::isfinite(value) && value >= 0 && value <= 9007199254740991.0 && std::floor(value) == value;
}

} // namespace

@interface DownstreamSession : NativeDownstreamSessionSpecBase <NativeDownstreamSessionSpec>
@end

@implementation DownstreamSession

RCT_EXPORT_MODULE(DownstreamSession)

+ (BOOL)requiresMainQueueSetup {
  return NO;
}

- (std::shared_ptr<TurboModule>)getTurboModule:(const ObjCTurboModule::InitParams &)params {
  return std::make_shared<NativeDownstreamSessionSpecJSI>(params);
}

- (void)emitErrorForSession:(NSString *)sessionId
                      code:(NSString *)code
                   message:(NSString *)message
                generation:(double)generation
                  sequence:(double)sequence {
  NSDictionary *payload = @{
    @"sessionId" : sessionId,
    @"code" : code,
    @"message" : message,
    @"generation" : @(validCounter(generation) ? generation : 0),
    @"sequence" : @(validCounter(sequence) ? sequence : 0),
    @"recoverable" : @YES,
  };
  dispatch_async(dispatch_get_main_queue(), ^{
    [self emitOnSessionError:payload];
  });
}

- (void)create:(NSString *)sessionId mode:(NSString *)mode {
  if (sessionId.length == 0 || !([mode isEqualToString:@"full"] || [mode isEqualToString:@"incremental"])) {
    [self emitErrorForSession:sessionId
                        code:@"invalid_session"
                     message:@"A nonempty session ID and valid parse mode are required"
                  generation:0
                    sequence:0];
    return;
  }

  const auto parseMode = [mode isEqualToString:@"incremental"] ? downstream::ParseMode::IncrementalTail
                                                              : downstream::ParseMode::FullParse;
  __weak DownstreamSession *weakSelf = self;
  try {
    const bool created = downstream::SessionRegistry::create(
        utf8(sessionId), parseMode, [weakSelf](const std::string &id, uint64_t generation, uint64_t sequence) {
          NSString *readyId = [[NSString alloc] initWithBytes:id.data() length:id.size() encoding:NSUTF8StringEncoding];
          if (!readyId) {
            return;
          }
          dispatch_async(dispatch_get_main_queue(), ^{
            DownstreamSession *strongSelf = weakSelf;
            if (strongSelf && downstream::SessionRegistry::snapshot(utf8(readyId), generation, sequence)) {
              [strongSelf emitOnSnapshotReady:@{
                @"sessionId" : readyId,
                @"generation" : @(generation),
                @"sequence" : @(sequence),
              }];
            }
          });
        });
    if (!created) {
      [self emitErrorForSession:sessionId
                          code:@"create_failed"
                       message:@"Session already exists or could not be created"
                    generation:0
                      sequence:0];
    }
  } catch (const std::exception &error) {
    [self emitErrorForSession:sessionId
                        code:@"create_failed"
                     message:[NSString stringWithUTF8String:error.what()]
                  generation:0
                    sequence:0];
  }
}

- (void)append:(NSString *)sessionId chunk:(NSString *)chunk generation:(double)generation sequence:(double)sequence {
  if (!validCounter(generation) || !validCounter(sequence)) {
    [self emitErrorForSession:sessionId
                        code:@"invalid_sequence"
                     message:@"Generation and sequence must be nonnegative integers"
                  generation:generation
                    sequence:sequence];
    return;
  }
  try {
    if (!downstream::SessionRegistry::append(utf8(sessionId), utf8(chunk), static_cast<uint64_t>(generation),
                                             static_cast<uint64_t>(sequence))) {
      [self emitErrorForSession:sessionId
                          code:@"append_failed"
                       message:@"Session is unavailable or append is out of order"
                    generation:generation
                      sequence:sequence];
    }
  } catch (const std::exception &error) {
    [self emitErrorForSession:sessionId
                        code:@"append_failed"
                     message:[NSString stringWithUTF8String:error.what()]
                  generation:generation
                    sequence:sequence];
  }
}

- (void)reset:(NSString *)sessionId source:(NSString *)source generation:(double)generation {
  if (!validCounter(generation)) {
    [self emitErrorForSession:sessionId
                        code:@"invalid_generation"
                     message:@"Generation must be a nonnegative integer"
                  generation:generation
                    sequence:0];
    return;
  }
  try {
    if (!downstream::SessionRegistry::reset(utf8(sessionId), utf8(source), static_cast<uint64_t>(generation))) {
      [self emitErrorForSession:sessionId
                          code:@"reset_failed"
                       message:@"Session is unavailable or generation is stale"
                    generation:generation
                      sequence:0];
    }
  } catch (const std::exception &error) {
    [self emitErrorForSession:sessionId
                        code:@"reset_failed"
                     message:[NSString stringWithUTF8String:error.what()]
                  generation:generation
                    sequence:0];
  }
}

- (void)finish:(NSString *)sessionId generation:(double)generation sequence:(double)sequence {
  if (!validCounter(generation) || !validCounter(sequence)) {
    [self emitErrorForSession:sessionId
                        code:@"invalid_sequence"
                     message:@"Generation and sequence must be nonnegative integers"
                  generation:generation
                    sequence:sequence];
    return;
  }
  try {
    if (!downstream::SessionRegistry::finish(utf8(sessionId), static_cast<uint64_t>(generation),
                                             static_cast<uint64_t>(sequence))) {
      [self emitErrorForSession:sessionId
                          code:@"finish_failed"
                       message:@"Session is unavailable or finish is out of order"
                    generation:generation
                      sequence:sequence];
    }
  } catch (const std::exception &error) {
    [self emitErrorForSession:sessionId
                        code:@"finish_failed"
                     message:[NSString stringWithUTF8String:error.what()]
                  generation:generation
                    sequence:sequence];
  }
}

- (void)dispose:(NSString *)sessionId {
  try {
    downstream::SessionRegistry::dispose(utf8(sessionId));
  } catch (const std::exception &error) {
    [self emitErrorForSession:sessionId
                        code:@"dispose_failed"
                     message:[NSString stringWithUTF8String:error.what()]
                  generation:0
                    sequence:0];
  }
}

@end
