#pragma once

#ifdef __cplusplus
#include "CodeLayout.h"
#include "ImageLayout.h"
#endif

#import <UIKit/UIKit.h>

#import "DSRenderedDocument.h"

NS_ASSUME_NONNULL_BEGIN

@protocol DSLinkDelegate <NSObject>
- (void)didPressLinkAtIndex:(NSInteger)linkIndex;
@end

/// Block kinds a control can act on, matching `BlockKind` in the engine. A
/// control reads a *range* of the document, so the kind says which block type
/// the range holds and the index says where that range starts; the two are
/// separate values and neither stands in for the other.
typedef NS_ENUM(NSInteger, DSBlockKind) {
  DSBlockKindCode = 2,
  DSBlockKindTableRow = 5,
  DSBlockKindImage = 7,
};

/// Formats a control can produce, matching `ExportFormat` in the engine.
typedef NS_ENUM(NSInteger, DSExportFormat) {
  DSExportFormatText = 0,
  DSExportFormatCsv = 1,
  DSExportFormatTsv = 2,
  DSExportFormatMarkdown = 3,
  DSExportFormatHtml = 4,
};

/// Something a control button did, for the view to report to JavaScript.
@protocol DSControlDelegate <NSObject>
/// A copy or a download of a code block or a table. `format` and `filename` are
/// empty when the action did not produce one.
- (void)didPressControlOfKind:(DSBlockKind)kind
                   blockIndex:(NSInteger)blockIndex
                   blockCount:(uint32_t)blockCount
                     language:(NSString *)language
                      action:(NSString *)action
                      format:(NSString *)format
                    filename:(NSString *)filename;
/// A per-block wrap toggle. `ordinal` is the block's ordinal among the
/// document's code blocks, which is what the choice is keyed by.
- (void)didToggleWrapAtOrdinal:(uint32_t)ordinal language:(NSString *)language wrapped:(BOOL)wrapped;
/// A per-block height-cap toggle, from the expand button.
- (void)didToggleExpandAtOrdinal:(uint32_t)ordinal language:(NSString *)language expanded:(BOOL)expanded;
/// A copy, a download, or a save that failed.
- (void)didFailControlOfKind:(DSBlockKind)kind action:(NSString *)action message:(NSString *)message;
@end

@protocol DSImageDelegate <NSObject>
/// A press on a loaded image that is not inside a link, so the app can open its
/// own viewer. The library opens nothing: a tap is the app's decision, because an
/// app that has a viewer wants it and one that does not does not.
- (void)didPressImageWithURL:(NSString *)url
                         alt:(NSString *)alt
                       title:(NSString *)title
                    ordinal:(uint32_t)ordinal;
@end

@protocol DSSegmentView <NSObject>
/// Link index at a point in the view's coordinates, or -1.
- (NSInteger)linkIndexAtPoint:(CGPoint)point;
@end

/// A text segment drawn by UITextView on the same TextKit 1 configuration
/// that measured it, which gives selection and text navigation.
@interface DSTextSegmentView : UITextView <DSSegmentView>
@property (nonatomic, weak, nullable) id<DSLinkDelegate> linkDelegate;
/// The theme's selection highlight. TextKit draws the selection itself, so this
/// is the only way a themed `colors.selection` reaches the platform's drawing.
@property (nonatomic) UIColor *selectionColor;
- (instancetype)init;
- (void)setSegment:(DSTextSegment *)segment;
@end

/// One code block: a header row above a card that scrolls sideways and, when the
/// block is past `codeBlockMaxHeight`, vertically inside its own frame.
///
/// The body is a text segment view, so selection, plain-text copy, token colors,
/// and link hit testing inside the code are the ones a text segment already has.
/// The gutter is a sibling that draws numbers rather than holding text, so there
/// is nothing to select, copy, or read aloud.
@interface DSCodeSegmentView : UIView <DSSegmentView>
@property (nonatomic, weak, nullable) id<DSLinkDelegate> linkDelegate;
@property (nonatomic, weak, nullable) id<DSControlDelegate> controlDelegate;
@property (nonatomic) BOOL controlsEnabled;
@property (nonatomic, readwrite) BOOL showsHeader;
/// Buttons to build, as bits of `downstream::ControlBits`.
@property (nonatomic) uint32_t controls;
/// The block's resolved layout, from the props, the reader's choices, and the
/// fence's own meta. One value, because a measurement and a draw both resolve it
/// rather than each re-deciding what wraps or what is capped.
@property (nonatomic) downstream::CodeBlockLayout layout;
/// The theme's selection highlight, applied to the body text view.
@property (nonatomic) UIColor *selectionColor;
- (void)setSegment:(DSCodeSegment *)segment;
- (void)setControlLabels:(NSArray<NSString *> *)labels;
@end

/// The vertical viewport around a code card's gutter and body.
///
/// Every code card has one, so a block that crosses the cap mid-stream is never
/// re-parented and keeps its selection and its scroll offsets. Vertical scrolling
/// is enabled only while the block is capped.
///
/// A drag that starts here belongs to this scroll view only while it can scroll
/// that way: at an edge it refuses, and the chat list's own pan takes the drag.
@interface DSCodeViewport : UIScrollView
/// True while this block's fence is still open, which is what makes a capped
/// card follow its newest line. A settled block keeps whatever offset it has.
@property (nonatomic) BOOL streaming;
/// Moves to the newest line after the next layout, unless the reader has scrolled
/// away from the bottom.
- (void)requestPin;
@end

/// One table: fixed column widths in a horizontal scroll view, with a toolbar
/// row above the grid and container bars and markers in the leading margin.
@interface DSTableSegmentView : UIView <DSSegmentView>
@property (nonatomic, weak, nullable) id<DSLinkDelegate> linkDelegate;
@property (nonatomic, weak, nullable) id<DSControlDelegate> controlDelegate;
@property (nonatomic) BOOL controlsEnabled;
@property (nonatomic, readwrite) BOOL showsToolbar;
@property (nonatomic) uint32_t controls;
- (void)setSegment:(DSTableSegment *)segment;
- (void)setControlLabels:(NSArray<NSString *> *)labels;
@end

/**
 * One image block: a stack of boxes, each a picture or the placeholder for it.
 *
 * The view draws what `imageBox` decided and nothing else, so a measured image
 * and a drawn one are the same size. A load that finishes records its result,
 * the shadow node measures again through a state update, and the box the view
 * draws is the box the measurement made.
 */
@interface DSImageSegmentView : UIView <DSSegmentView>
@property (nonatomic, weak, nullable) id<DSLinkDelegate> linkDelegate;
@property (nonatomic, weak, nullable) id<DSControlDelegate> controlDelegate;
@property (nonatomic, weak, nullable) id<DSImageDelegate> imageDelegate;
@property (nonatomic) BOOL controlsEnabled;
@property (nonatomic) uint32_t controls;
/// Label strings in the order `src/controls.ts` sends them.
- (void)setControlLabels:(NSArray<NSString *> *)labels;
/// The state each image is in, which is what the boxes are drawn from.
- (void)setImageState:(const downstream::ImageStateLayout &)state;
/// True when this block is settled, which is what turns a control on and stops a
/// pending box from flashing.
@property (nonatomic) BOOL settled;
/// The screen scale the pictures were decoded at, which is what a press's
/// download name and a re-request need.
@property (nonatomic) CGFloat screenScale;
- (void)setSegment:(DSImageSegment *)segment;
@end

/**
 * One display formula: a horizontal scroll view around a view that draws vectors.
 *
 * A formula wider than the column scrolls rather than being scaled into it, which
 * is what KaTeX does and what keeps a wide matrix readable. The drag rules are
 * the code card's: this scroll view takes a horizontal drag while it can scroll
 * that way and refuses at the edge, so a chat list's own pan takes over.
 */
@interface DSMathSegmentView : UIView <DSSegmentView>
@property (nonatomic) BOOL streaming;
- (void)setSegment:(DSMathSegment *)segment;
@end

/// Stacks segment views at the frames the measurement computed.
@interface DSDocumentView : UIView
@property (nonatomic, weak, nullable) id<DSLinkDelegate> linkDelegate;
@property (nonatomic, weak, nullable) id<DSControlDelegate> controlDelegate;
@property (nonatomic) BOOL selectable;
@property (nonatomic) uint32_t controls;
/// The whole code layout: the `lineNumbers`, `codeBlockWrap`, and
/// `codeBlockMaxHeight` props, plus the reader's own per-block wrap and expand
/// choices. One value, so a frame and the layout drawn inside it cannot disagree.
@property (nonatomic) downstream::CodeLayout codeLayout;
/// Label strings in the order `src/controls.ts` sends them.
- (void)setControlLabels:(NSArray<NSString *> *)labels;
- (void)setRendered:(nullable DSRenderedDocument *)rendered;
- (void)setChrome:(uint32_t)chrome;
/// Drops every reader scroll position, for a new generation.
- (void)resetScrollPositions;
- (NSInteger)linkIndexAtPoint:(CGPoint)point;
/// The selection highlight, from the view's theme. Applied to every text view,
/// so a themed `colors.selection` reaches the platform's own selection drawing
/// rather than only the parts this library draws itself.
@property (nonatomic) UIColor *selectionColor;
@end

NS_ASSUME_NONNULL_END
