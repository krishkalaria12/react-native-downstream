#pragma once

#ifdef __cplusplus
#include "CodeLayout.h"
#include "HostLayout.h"
#include "ImageLayout.h"
#endif

#import <UIKit/UIKit.h>

#import "DSLayoutManager.h"
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

#pragma mark - Shared by the segment view implementations

/**
 * The pieces more than one segment view is built from.
 *
 * Each implementation lives beside the view that owns it — a code card's gutter
 * beside the card, a table's grid beside the table — so this header carries what
 * crosses between them: the control buttons a code header and a table toolbar are
 * both made of, and the link hit testing and accessibility slicing a text
 * segment and a table grid both answer.
 */

/// The height of a control row, matching the one the measurement reserves.
extern const CGFloat kChromeRowHeight;

/// How far outside a glyph a press still counts as its link.
extern const CGFloat kLinkHitSlop;

/// Indexes into the label array `src/controls.ts` sends, in the same order.
typedef NS_ENUM(NSInteger, DSControlLabel) {
  DSControlLabelCopyCode = 0,
  DSControlLabelDownloadFile = 1,
  DSControlLabelCopied = 2,
  DSControlLabelWrapCode = 3,
  DSControlLabelUnwrapCode = 4,
  // Appended, not inserted: `src/controls.ts` sends the labels positionally, so
  // a new key goes at the end rather than moving every later one.
  DSControlLabelExpandCode = 5,
  DSControlLabelCollapseCode = 6,
  DSControlLabelCopyTable = 7,
  DSControlLabelCopyTableAsMarkdown = 8,
  DSControlLabelCopyTableAsCsv = 9,
  DSControlLabelCopyTableAsTsv = 10,
  DSControlLabelDownloadTable = 11,
  DSControlLabelDownloadTableAsCsv = 12,
  DSControlLabelDownloadTableAsMarkdown = 13,
  DSControlLabelTableFormatMarkdown = 14,
  DSControlLabelTableFormatCsv = 15,
  DSControlLabelTableFormatTsv = 16,
  // Appended, in `src/controls.ts`'s order. The four image keys: what a failed
  // image says, what its button says, and the two names a screen reader needs
  // when the alt text is empty or the prefix list blocked it.
  DSControlLabelImageNotAvailable = 17,
  DSControlLabelDownloadImage = 18,
  DSControlLabelImage = 19,
  DSControlLabelImageBlocked = 20,
  DSControlLabelCount = 21,
};

extern NSString *const kSymbolCopy;
extern NSString *const kSymbolDownload;

/// One action on a code block or a table. The view that owns the button sends
/// the delegate a description rather than doing the work itself, because only
/// the view that holds the document can read a block.
@interface DSControlButton : UIButton
@property (nonatomic, copy) NSString *action;
@property (nonatomic, copy) NSString *format;
@end

/// Every control glyph at one size, so a glyph swapped in later (a copy's check,
/// a wrap state) matches the ones built with the button.
UIImage *controlImage(NSString *symbol);
UIButton *makeControlButton(NSString *symbol, NSString *label, NSString *action, NSString *format);
NSString *symbolForCopy(BOOL copied);
NSString *symbolForWrap(BOOL wrapped);
NSString *symbolForExpand(BOOL expanded);

/// Whether a laid-out range carries a link, which is what decides whether a
/// reader hears one element or several.
BOOL hasLink(NSAttributedString *text, NSRange range);

/// Link index under a point in a laid-out TextKit stack, or -1.
NSInteger linkIndexInStack(NSLayoutManager *layoutManager, NSTextContainer *container, CGPoint location);

/// Detaches the link delegates a view's cached elements hold, so a recycled view
/// cannot be activated through an element that outlived its text.
void invalidateAccessibleLinks(NSArray<UIAccessibilityElement *> *elements);

/// Splits only ranges containing links. Each link gets a focusable element in
/// source order; plain text remains readable without repeating link labels.
void appendAccessibleSlices(NSMutableArray<UIAccessibilityElement *> *elements, NSAttributedString *text,
                            NSRange range, UIView *container, NSLayoutManager *layoutManager,
                            NSTextContainer *textContainer, CGPoint origin, CGRect fallbackFrame,
                            NSString *_Nullable prefix, UIAccessibilityTraits firstTraits,
                            id<DSLinkDelegate> _Nullable delegate);

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
/// A load finished and its answer is in the engine's size cache. The host asks
/// the shadow node to measure again, which is how a picture arriving makes the
/// message taller without a React render.
- (void)imagesDidChange;
@end

@protocol DSSegmentView <NSObject>
/// Link index at a point in the view's coordinates, or -1.
- (NSInteger)linkIndexAtPoint:(CGPoint)point;
@end

/// A segment view that can be told to redraw one block's characters.
///
/// The range is in the *document block's* coordinates rather than the view's own,
/// because the ticker works in the engine's units and translating into each
/// view's text would mean walking every segment's block ranges on every tick.
@protocol DSRevealInvalidatable <NSObject>
- (void)invalidateRevealCharacterRange:(NSRange)range
                            blockOffset:(NSInteger)blockOffset
                                     now:(double)now;
@end

/// A text segment drawn by UITextView on the same TextKit 1 configuration
/// that measured it, which gives selection and text navigation.
@interface DSTextSegmentView : UITextView <DSSegmentView, DSRevealInvalidatable>
@property (nonatomic, weak, nullable) id<DSLinkDelegate> linkDelegate;
/// The theme's selection highlight. TextKit draws the selection itself, so this
/// is the only way a themed `colors.selection` reaches the platform's drawing.
@property (nonatomic) UIColor *selectionColor;
/// Where this view's fading characters are. Weak, so a recycled view cannot keep
/// a document alive through a fade.
@property (nonatomic, weak, nullable) id<DSRevealProvider> revealProvider;
/// The document block this view's first character belongs to, which is what maps
/// a reveal unit's block-and-offset pair into this view's own text.
@property (nonatomic) NSInteger blockOffset;
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
@interface DSCodeSegmentView : UIView <DSSegmentView, DSRevealInvalidatable>
@property (nonatomic, weak, nullable) id<DSLinkDelegate> linkDelegate;
@property (nonatomic, weak, nullable) id<DSControlDelegate> controlDelegate;
/// The block this card draws, so the document view resolves its layout against
/// the same meta the measurement used.
@property (nonatomic, readonly, nullable) DSCodeSegment *segment;
/// False once the block's fence has closed, which stops a capped card following
/// its newest line.
@property (nonatomic) BOOL streaming;
/// True while a React child draws this block instead of the card.
///
/// The card's background, header, gutter and body all belong to a fallback that
/// is not being shown, and a hosted block draws only what the child cannot: the
/// quote and admonition bars beside it and the list marker on its first line,
/// over the full height the child measured.
@property (nonatomic) BOOL hosted;
/// Where the code body's fading alphas come from.
@property (nonatomic, weak, nullable) id<DSRevealProvider> revealProvider;
/// The document block this code card draws.
@property (nonatomic) NSInteger blockOffset;
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
/// Drops the reader's scroll position when the document generation changes.
- (void)resetScrollPosition;
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
@interface DSTableSegmentView : UIView <DSSegmentView, DSRevealInvalidatable>
@property (nonatomic, weak, nullable) id<DSLinkDelegate> linkDelegate;
/// Where the rows' fading alphas come from.
@property (nonatomic, weak, nullable) id<DSRevealProvider> revealProvider;
/// The document block this table's first row is.
@property (nonatomic) NSInteger blockOffset;
@property (nonatomic, weak, nullable) id<DSControlDelegate> controlDelegate;
@property (nonatomic) BOOL controlsEnabled;
@property (nonatomic, readwrite) BOOL showsToolbar;
@property (nonatomic) uint32_t controls;
- (void)setSegment:(DSTableSegment *)segment;
- (void)setControlLabels:(NSArray<NSString *> *)labels;
/// Drops the reader's scroll position when the document generation changes.
- (void)resetScrollPosition;
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
/// Where the document's fading characters are. Weak, because the provider is the
/// Markdown view's and this one is recycled with its segments.
@property (nonatomic, weak, nullable) id<DSRevealProvider> revealProvider;
/// The segment views that can fade, so a tick invalidates only the lines holding
/// an active unit rather than the whole document.
- (nullable UIView<DSRevealInvalidatable> *)revealViewForBlock:(NSInteger)block;
/// Coalesces active character units into one display invalidation per block.
- (void)invalidateRevealUnits:(std::span<const downstream::RevealUnit>)units
                    document:(const downstream::Document &)document
                         now:(double)now;
- (NSArray<UIView<DSRevealInvalidatable> *> *)revealViewsForBlock:(NSInteger)block;
@property (nonatomic, weak, nullable) id<DSLinkDelegate> linkDelegate;
@property (nonatomic, weak, nullable) id<DSControlDelegate> controlDelegate;
@property (nonatomic, weak, nullable) id<DSImageDelegate> imageDelegate;
@property (nonatomic) BOOL selectable;
@property (nonatomic) uint32_t controls;
/// The whole code layout: the `lineNumbers`, `codeBlockWrap`, and
/// `codeBlockMaxHeight` props, plus the reader's own per-block wrap and expand
/// choices. One value, so a frame and the layout drawn inside it cannot disagree.
@property (nonatomic) downstream::CodeLayout codeLayout;
/// What each image is, which the image views draw from and the frames are
/// stacked with.
@property (nonatomic) downstream::ImageStateLayout imageState;
/// Which fenced blocks a React child replaced, and how tall each one is.
///
/// A code segment whose ordinal is in here is drawn as a host segment: its
/// container chrome and nothing else, at the height the child measured, with
/// the child itself drawn above it by Fabric. An empty layout is a view with no
/// renderers, which is the state this view was in before fence renderers
/// existed.
@property (nonatomic) downstream::HostLayout hostLayout;
/// The screen scale pictures are decoded at.
@property (nonatomic) CGFloat screenScale;
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
