#import "DSSegmentViewport.h"
#include <algorithm>

@implementation DSDeferredTextView
- (NSInteger)linkIndexAtPoint:(CGPoint)point { return -1; }
@end

@interface DSDocumentView (Viewport)
- (void)refreshSegmentViewport;
@end

// One display link checks actual clipping ancestors for all participating
// document views. Weak registration does not keep recycled Fabric views alive.
@interface DSSegmentViewportClock : NSObject
@property (nonatomic, strong) NSHashTable<DSDocumentView *> *views;
@property (nonatomic, strong) CADisplayLink *link;
+ (instancetype)shared;
- (void)track:(DSDocumentView *)view enabled:(BOOL)enabled;
@end
@implementation DSSegmentViewportClock
+ (instancetype)shared {
  static DSSegmentViewportClock *clock;
  static dispatch_once_t once;
  dispatch_once(&once, ^{ clock = [DSSegmentViewportClock new]; clock.views = [NSHashTable weakObjectsHashTable]; });
  return clock;
}
- (void)track:(DSDocumentView *)view enabled:(BOOL)enabled {
  if (enabled) [self.views addObject:view]; else [self.views removeObject:view];
  if (self.views.count > 0 && self.link == nil) {
    self.link = [CADisplayLink displayLinkWithTarget:self selector:@selector(tick:)];
    [self.link addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
  } else if (self.views.count == 0) {
    [self.link invalidate]; self.link = nil;
  }
}
- (void)tick:(CADisplayLink *)link {
  for (DSDocumentView *view in self.views.allObjects) [view refreshSegmentViewport];
  if (self.views.count == 0) { [self.link invalidate]; self.link = nil; }
}
@end

void DSObserveSegmentViewport(DSDocumentView *view, BOOL enabled) {
  [[DSSegmentViewportClock shared] track:view enabled:enabled];
}

BOOL DSCanDeferTextSegment(DSTextSegment *segment) {
  __block BOOL eligible = segment.customContainer == UINT32_MAX;
  for (DSBlockInfo *block in segment.blocks) {
    if (block.containers.count != 0) return NO;
  }
  if (eligible) [segment.attributedString enumerateAttributesInRange:NSMakeRange(0, segment.attributedString.length)
      options:0 usingBlock:^(NSDictionary *attributes, NSRange range, BOOL *stop) {
        if (attributes[DSLinkIndexAttributeName] || attributes[DSTagIndexAttributeName] ||
            attributes[DSSpoilerAttributeName] || attributes[DSCaretKindAttributeName] ||
            attributes[NSAttachmentAttributeName]) { eligible = NO; *stop = YES; }
      }];
  return eligible;
}

CGRect DSSegmentVisibleRect(UIView *view) {
  CGRect visible = view.bounds;
  for (UIView *ancestor = view; ancestor != nil; ancestor = ancestor.superview) {
    if (ancestor.hidden || ancestor.alpha == 0) return CGRectZero;
    if (ancestor.clipsToBounds || ancestor == view.window) {
      visible = CGRectIntersection(visible, [view convertRect:ancestor.bounds fromView:ancestor]);
      if (CGRectIsNull(visible)) return CGRectZero;
    }
  }
  return visible;
}

NSRange DSSegmentVisibleRange(NSArray<NSValue *> *frames, CGRect viewport) {
  if (CGRectIsEmpty(viewport)) return NSMakeRange(0, 0);
  const CGFloat overscan = std::max(CGFloat(64), viewport.size.height / 2);
  const CGFloat top = CGRectGetMinY(viewport) - overscan, bottom = CGRectGetMaxY(viewport) + overscan;
  NSUInteger low = 0, high = frames.count;
  while (low < high) {
    const NSUInteger middle = (low + high) / 2;
    if (CGRectGetMaxY(frames[middle].CGRectValue) < top) low = middle + 1; else high = middle;
  }
  const NSUInteger first = low;
  high = frames.count;
  while (low < high) {
    const NSUInteger middle = (low + high) / 2;
    if (CGRectGetMinY(frames[middle].CGRectValue) <= bottom) low = middle + 1; else high = middle;
  }
  return NSMakeRange(first, low - first);
}

BOOL DSSegmentFramesAreOrdered(NSArray<NSValue *> *frames) {
  for (NSUInteger index = 1; index < frames.count; index++) {
    const CGRect previous = frames[index - 1].CGRectValue, current = frames[index].CGRectValue;
    if (CGRectGetMinY(current) < CGRectGetMinY(previous) || CGRectGetMaxY(current) < CGRectGetMaxY(previous)) return NO;
  }
  return YES;
}
