#pragma once

#import <UIKit/UIKit.h>

#import "DSRenderedDocument.h"

NS_ASSUME_NONNULL_BEGIN

@protocol DSLinkDelegate <NSObject>
- (void)didPressLinkAtIndex:(NSInteger)linkIndex;
@end

@protocol DSSegmentView <NSObject>
/// Link index at a point in the view's coordinates, or -1.
- (NSInteger)linkIndexAtPoint:(CGPoint)point;
@end

/// A text segment drawn by UITextView on the same TextKit 1 configuration
/// that measured it, which gives selection and text navigation.
@interface DSTextSegmentView : UITextView <DSSegmentView>
@property (nonatomic, weak, nullable) id<DSLinkDelegate> linkDelegate;
- (instancetype)init;
- (void)setSegment:(DSTextSegment *)segment;
@end

/// One table: fixed column widths in a horizontal scroll view, with
/// container bars and markers in the leading margin.
@interface DSTableSegmentView : UIView <DSSegmentView>
@property (nonatomic, weak, nullable) id<DSLinkDelegate> linkDelegate;
- (void)setSegment:(DSTableSegment *)segment;
@end

/// Stacks segment views at the frames the measurement computed.
@interface DSDocumentView : UIView
@property (nonatomic, weak, nullable) id<DSLinkDelegate> linkDelegate;
@property (nonatomic) BOOL selectable;
- (void)setRendered:(nullable DSRenderedDocument *)rendered;
- (NSInteger)linkIndexAtPoint:(CGPoint)point;
@end

NS_ASSUME_NONNULL_END
