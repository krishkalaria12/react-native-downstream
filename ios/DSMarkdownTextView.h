#pragma once

#import <UIKit/UIKit.h>

#ifdef __cplusplus
#include "Document.h"
#endif

NS_ASSUME_NONNULL_BEGIN

@class DSMarkdownTextView;

@protocol DSMarkdownTextViewDelegate <NSObject>
- (void)markdownTextView:(DSMarkdownTextView *)view didPressLinkAtIndex:(NSInteger)linkIndex;
@end

/// Draws a rendered document with TextKit and exposes its blocks and links
/// to assistive technologies.
@interface DSMarkdownTextView : UIView

@property (nonatomic, weak, nullable) id<DSMarkdownTextViewDelegate> delegate;

#ifdef __cplusplus
/// Applies a document measured with the given font scale. Same values are a
/// no-op, so repeated state updates do not redraw.
- (void)setDocument:(downstream::DocumentHandle)document fontScale:(CGFloat)fontScale;
#endif

/// Link index at a point in the view's coordinates, or -1.
- (NSInteger)linkIndexAtPoint:(CGPoint)point;

@end

NS_ASSUME_NONNULL_END
