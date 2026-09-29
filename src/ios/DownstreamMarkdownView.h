#pragma once

#import <React/RCTViewComponentView.h>
#import <UIKit/UIKit.h>

NS_ASSUME_NONNULL_BEGIN

/// Fabric host view for <DownstreamMarkdown>. Renders the document carried by
/// the shadow node's state; it never parses Markdown itself.
@interface DownstreamMarkdownView : RCTViewComponentView
@end

NS_ASSUME_NONNULL_END
