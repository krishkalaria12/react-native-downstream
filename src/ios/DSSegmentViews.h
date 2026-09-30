#pragma once

#import <UIKit/UIKit.h>

#import "DSRenderedDocument.h"

NS_ASSUME_NONNULL_BEGIN

@protocol DSLinkDelegate <NSObject>
- (void)didPressLinkAtIndex:(NSInteger)linkIndex;
@end

/// Something a control button did, for the view to report to JavaScript.
@protocol DSControlDelegate <NSObject>
/// A copy or a download of a code block or a table. `format` and `filename` are
/// empty when the action did not produce one.
- (void)didPressControlOfKind:(NSInteger)blockIndex
                      blockCount:(uint32_t)blockCount
                        language:(NSString *)language
                         action:(NSString *)action
                         format:(NSString *)format
                       filename:(NSString *)filename;
/// A per-block wrap toggle. `ordinal` is the block's ordinal among the
/// document's code blocks, which is what the choice is keyed by.
- (void)didToggleWrapAtOrdinal:(uint32_t)ordinal language:(NSString *)language wrapped:(BOOL)wrapped;
/// A copy, a download, or a save that failed.
- (void)didFailControlOfKind:(NSInteger)blockIndex action:(NSString *)action message:(NSString *)message;
@end

/// Block kinds, matching `BlockKind` in the engine. A control reads a range of
/// the document, so it names the kind it is acting on.
typedef NS_ENUM(NSInteger, DSBlockKind) {
  DSBlockKindCode = 2,
  DSBlockKindTableRow = 5,
};

typedef NS_ENUM(NSInteger, DSControlAction) {
  DSControlActionCopy = 0,
  DSControlActionDownload = 1,
  DSControlActionWrap = 2,
};

/// Formats a control can produce, matching `ExportFormat` in the engine.
typedef NS_ENUM(NSInteger, DSExportFormat) {
  DSExportFormatText = 0,
  DSExportFormatCsv = 1,
  DSExportFormatTsv = 2,
  DSExportFormatMarkdown = 3,
  DSExportFormatHtml = 4,
};

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

/// One code block: a header row above a horizontally scrolling body. The body
/// is a text segment view, so selection, plain-text copy, token colors, and
/// link hit testing inside the code are the ones a text segment already has.
@interface DSCodeSegmentView : UIView <DSSegmentView>
@property (nonatomic, weak, nullable) id<DSLinkDelegate> linkDelegate;
@property (nonatomic, weak, nullable) id<DSControlDelegate> controlDelegate;
@property (nonatomic) BOOL controlsEnabled;
@property (nonatomic, readwrite) BOOL showsHeader;
/// Buttons to build, as bits of `downstream::ControlBits`.
@property (nonatomic) uint32_t controls;
/// The block's current layout: wrapping or scrolling sideways.
@property (nonatomic, getter=isWrapped) BOOL wrapped;
- (void)setSegment:(DSCodeSegment *)segment;
- (void)setControlLabels:(NSArray<NSString *> *)labels;
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

/// Stacks segment views at the frames the measurement computed.
@interface DSDocumentView : UIView
@property (nonatomic, weak, nullable) id<DSLinkDelegate> linkDelegate;
@property (nonatomic, weak, nullable) id<DSControlDelegate> controlDelegate;
@property (nonatomic) BOOL selectable;
@property (nonatomic) uint32_t controls;
@property (nonatomic) BOOL codeBlockWrap;
/// Code blocks whose layout differs from `codeBlockWrap`, by ordinal. A block
/// wraps when that is true, and scrolls sideways when it is not.
@property (nonatomic, copy) NSSet<NSNumber *> *codeWrapToggles;
/// Label strings in the order `src/controls.ts` sends them.
- (void)setControlLabels:(NSArray<NSString *> *)labels;
- (void)setRendered:(nullable DSRenderedDocument *)rendered;
- (void)setChrome:(uint32_t)chrome;
- (void)resetTableScrollPositions;
- (NSInteger)linkIndexAtPoint:(CGPoint)point;
@end

NS_ASSUME_NONNULL_END
