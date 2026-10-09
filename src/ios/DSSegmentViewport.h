#pragma once
#import "DSSegmentViews.h"

/// Index slot without an offscreen TextKit stack.
@interface DSDeferredTextView : UIView <DSSegmentView>
@end

/// Internal viewport policy; never changes document geometry or source data.
BOOL DSCanDeferTextSegment(DSTextSegment *segment);
CGRect DSSegmentVisibleRect(UIView *view);
NSRange DSSegmentVisibleRange(NSArray<NSValue *> *frames, CGRect viewport);
BOOL DSSegmentFramesAreOrdered(NSArray<NSValue *> *frames);
void DSObserveSegmentViewport(DSDocumentView *view, BOOL enabled);
