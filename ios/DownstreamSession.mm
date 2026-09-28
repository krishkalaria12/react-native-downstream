#import <Foundation/Foundation.h>

#import <DownstreamSpec/DownstreamSpec.h>
#import <React/RCTBridgeModule.h>
#import <React/RCTInvalidating.h>

#include "SessionRegistry.h"

#include <cmath>
#include <cstdint>
#include <exception>
#include <mutex>
#include <string>
#include <unordered_set>

using namespace facebook::react;

namespace {

std::string utf8(NSString *value) {
  NSData *encoded = [value dataUsingEncoding:NSUTF8StringEncoding];
  if (encoded.length == 0) {
    return {};
  }
  return std::string(static_cast<const char *>(encoded.bytes), encoded.length);
}

bool validCounter(double value) {
  return std::isfinite(value) && value >= 0 && value <= 9007199254740991.0 && std::floor(value) == value;
}

} // namespace

@interface DownstreamSession : NativeDownstreamSessionSpecBase <NativeDownstreamSessionSpec, RCTInvalidating>
@end

@implementation DownstreamSession {
  std::mutex _sessionsMutex;
  std::unordered_set<std::string> _sessions;
  bool _invalidated;
}

RCT_EXPORT_MODULE(DownstreamSession)

- (instancetype)init {
  if (self = [super init]) {
    _invalidated = false;
  }
  return self;
}

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
  const std::string id = utf8(sessionId);
  __weak DownstreamSession *weakSelf = self;
  try {
    bool created = false;
    {
      std::lock_guard<std::mutex> lock(_sessionsMutex);
      if (!_invalidated && _sessions.insert(id).second) {
        try {
          created = downstream::SessionRegistry::create(
              id, parseMode, [weakSelf](const std::string &readySessionId, uint64_t generation, uint64_t sequence) {
                NSString *readyId = [[NSString alloc] initWithBytes:readySessionId.data()
                                                             length:readySessionId.size()
                                                           encoding:NSUTF8StringEncoding];
                if (!readyId) {
                  return;
                }
                dispatch_async(dispatch_get_main_queue(), ^{
                  DownstreamSession *strongSelf = weakSelf;
                  if (!strongSelf) {
                    return;
                  }
                  const std::string id = utf8(readyId);
                  bool current = false;
                  {
                    std::lock_guard<std::mutex> readyLock(strongSelf->_sessionsMutex);
                    current = !strongSelf->_invalidated && strongSelf->_sessions.count(id) != 0 &&
                        downstream::SessionRegistry::snapshot(id, generation, sequence) != nullptr;
                  }
                  if (current) {
                    [strongSelf emitOnSnapshotReady:@{
                      @"sessionId" : readyId,
                      @"generation" : @(generation),
                      @"sequence" : @(sequence),
                    }];
                  }
                });
              });
        } catch (...) {
          _sessions.erase(id);
          throw;
        }
        if (!created) {
          _sessions.erase(id);
        }
      }
    }
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
  const std::string id = utf8(sessionId);
  try {
    bool appended = false;
    {
      std::lock_guard<std::mutex> lock(_sessionsMutex);
      if (!_invalidated && _sessions.count(id) != 0) {
        appended = downstream::SessionRegistry::append(id, utf8(chunk), static_cast<uint64_t>(generation),
                                                       static_cast<uint64_t>(sequence));
      }
    }
    if (!appended) {
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
  const std::string id = utf8(sessionId);
  try {
    bool reset = false;
    {
      std::lock_guard<std::mutex> lock(_sessionsMutex);
      if (!_invalidated && _sessions.count(id) != 0) {
        reset = downstream::SessionRegistry::reset(id, utf8(source), static_cast<uint64_t>(generation));
      }
    }
    if (!reset) {
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
  const std::string id = utf8(sessionId);
  try {
    bool finished = false;
    {
      std::lock_guard<std::mutex> lock(_sessionsMutex);
      if (!_invalidated && _sessions.count(id) != 0) {
        finished = downstream::SessionRegistry::finish(id, static_cast<uint64_t>(generation),
                                                       static_cast<uint64_t>(sequence));
      }
    }
    if (!finished) {
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
    const std::string id = utf8(sessionId);
    std::lock_guard<std::mutex> lock(_sessionsMutex);
    if (_sessions.count(id) != 0) {
      downstream::SessionRegistry::dispose(id);
      _sessions.erase(id);
    }
  } catch (const std::exception &error) {
    [self emitErrorForSession:sessionId
                        code:@"dispose_failed"
                     message:[NSString stringWithUTF8String:error.what()]
                  generation:0
                    sequence:0];
  }
}

- (void)invalidate {
  std::lock_guard<std::mutex> lock(_sessionsMutex);
  _invalidated = true;
  for (const std::string &id : _sessions) {
    downstream::SessionRegistry::dispose(id);
  }
  _sessions.clear();
}

- (void)dealloc {
  [self invalidate];
}

@end
