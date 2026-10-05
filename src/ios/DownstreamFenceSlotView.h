#pragma once

#import <React/RCTViewComponentView.h>
#import <UIKit/UIKit.h>

NS_ASSUME_NONNULL_BEGIN

/// Fabric host view for <DownstreamFenceSlot>, the child one hosted fence lives
/// in. A plain view: the renderer's own component is everything inside it.
@interface DownstreamFenceSlotView : RCTViewComponentView
@end

NS_ASSUME_NONNULL_END
