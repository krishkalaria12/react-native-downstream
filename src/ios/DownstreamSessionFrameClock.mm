#import <UIKit/UIKit.h>
#import <QuartzCore/QuartzCore.h>

#include "DownstreamSessionFrameClock.h"
#include "FrameScheduler.h"

// A single process-wide display link. It sleeps whenever there is no queued
// native session work, and never parses or prepares text on the main thread.
@interface DownstreamSessionFrameClock : NSObject
+ (instancetype)shared;
- (void)requestFrame;
@end

@implementation DownstreamSessionFrameClock {
  CADisplayLink *_displayLink;
  BOOL _active;
}

+ (instancetype)shared {
  static DownstreamSessionFrameClock *clock;
  static dispatch_once_t once;
  dispatch_once(&once, ^{ clock = [DownstreamSessionFrameClock new]; });
  return clock;
}

- (instancetype)init {
  self = [super init];
  if (self) {
    _active = [UIApplication sharedApplication].applicationState == UIApplicationStateActive;
    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(inactive:)
        name:UIApplicationWillResignActiveNotification object:nil];
    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(active:)
        name:UIApplicationDidBecomeActiveNotification object:nil];
  }
  return self;
}

- (void)inactive:(NSNotification *)notification {
  _active = NO;
  if (_displayLink != nil && !_displayLink.paused) {
    _displayLink.paused = YES;
    // A requested display tick must not strand an explicit flush or finish.
    dispatch_async(dispatch_get_main_queue(), ^{ [self requestFrame]; });
  }
}

- (void)active:(NSNotification *)notification {
  _active = YES;
}

- (void)requestFrame {
  if (!_active) {
    // requestFrame is dispatched asynchronously by the native requester.
    // Without display ticks, retain the previous headless progress behavior.
    downstream::FrameScheduler::onFrame();
    return;
  }
  if (_displayLink == nil) {
    _displayLink = [CADisplayLink displayLinkWithTarget:self selector:@selector(tick:)];
    [_displayLink addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
  }
  _displayLink.paused = NO;
}

- (void)tick:(CADisplayLink *)link {
  if (link.paused) return;
  link.paused = YES;
  const double interval = link.targetTimestamp - link.timestamp;
  // timestamp names the preceding display; a normal callback is already up
  // to one interval past it. Count only lateness beyond that interval.
  const bool late = CACurrentMediaTime() - link.timestamp > interval * 1.75;
  downstream::FrameScheduler::onFrame(interval * 1000.0, late);
}

@end

namespace downstream {

void installSessionFrameClock() {
  FrameScheduler::install([] {
    dispatch_async(dispatch_get_main_queue(), ^{
      [[DownstreamSessionFrameClock shared] requestFrame];
    });
  });
}

} // namespace downstream
