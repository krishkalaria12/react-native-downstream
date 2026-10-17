#import "DSSegmentViews.h"

#import "DSLayoutManager.h"
#import "DSRenderedDocument.h"

#include "Controls.h"
#include "TableLayout.h"

#include <algorithm>
#include <vector>

using namespace downstream::ControlBits;
// The slot names a themed drawing reads. The layout helpers they sit beside live
// in `theme_layout`, which needs no `using` here: the metrics arrive on the
// segment rather than as a rule to apply.
using namespace downstream::theme;

/// How far from the newest row a reader can be and still count as following it,
/// in points. The same eight the code card and the Android table use, because it
/// is one rule about one reader, not three.
static const CGFloat kTablePinThreshold = 8;

#pragma mark - Tables

@interface DSTableGridView : UIView
@property (nonatomic, nullable) DSTableSegment *segment;
/// Draws only the header row, which is what the pinned strip is: the same grid,
/// one row, so the two cannot disagree about a column's width.
@property (nonatomic) BOOL headerOnly;
@property (nonatomic, weak, nullable) id<DSLinkDelegate> linkDelegate;
/// Where the rows' fading alphas come from. A row is one reveal unit, so a new
/// row brings its fill, its cells, and its grid lines with it.
@property (nonatomic, weak, nullable) id<DSRevealProvider> revealProvider;
/// The document block this table's first row is.
@property (nonatomic) NSInteger blockOffset;
- (NSInteger)linkIndexAtPoint:(CGPoint)point;
- (void)invalidateRevealRow:(NSUInteger)row visibleRect:(CGRect)visibleRect;
@end

@implementation DSTableGridView {
  NSArray *_cachedAccessibilityElements;
  std::vector<CGFloat> _rowOffsets;
  std::vector<CGFloat> _columnOffsets;
}

- (instancetype)initWithFrame:(CGRect)frame {
  if (self = [super initWithFrame:frame]) {
    self.opaque = NO;
    self.backgroundColor = UIColor.clearColor;
    self.contentMode = UIViewContentModeRedraw;
  }
  return self;
}

- (void)setSegment:(DSTableSegment *)segment {
  if (segment == _segment) {
    return;
  }
  _segment = segment;
  _rowOffsets.assign(1, 0);
  _rowOffsets.reserve(segment.rowHeights.count + 1);
  for (NSNumber *height in segment.rowHeights) {
    _rowOffsets.push_back(_rowOffsets.back() + height.doubleValue);
  }
  _columnOffsets.assign(1, 0);
  _columnOffsets.reserve(segment.columnWidths.count + 1);
  for (NSNumber *width in segment.columnWidths) {
    _columnOffsets.push_back(_columnOffsets.back() + width.doubleValue);
  }
  invalidateAccessibleLinks(_cachedAccessibilityElements);
  _cachedAccessibilityElements = nil;
  [self setNeedsDisplay];
}

- (void)setLinkDelegate:(id<DSLinkDelegate>)linkDelegate {
  _linkDelegate = linkDelegate;
  invalidateAccessibleLinks(_cachedAccessibilityElements);
  _cachedAccessibilityElements = nil;
}

- (CGRect)contentRectForRow:(NSUInteger)row column:(NSUInteger)column {
  const CGFloat x = _segment.rightToLeft
      ? _columnOffsets.back() - _columnOffsets[column + 1]
      : _columnOffsets[column];
  const CGFloat y = _rowOffsets[row];
  const UIEdgeInsets padding = _segment.cellPadding;
  return CGRectMake(x + padding.left, y + padding.top,
                    _segment.columnWidths[column].doubleValue - padding.left - padding.right,
                    _segment.rowHeights[row].doubleValue - padding.top - padding.bottom);
}

- (void)invalidateRevealRow:(NSUInteger)row visibleRect:(CGRect)visibleRect {
  if (row >= _segment.rows.count || row + 1 >= _rowOffsets.size()) {
    return;
  }
  const CGRect rowRect = CGRectMake(0, _rowOffsets[row], _segment.tableSize.width,
                                    _rowOffsets[row + 1] - _rowOffsets[row]);
  const CGRect dirty = CGRectIntersection(rowRect, visibleRect);
  if (!CGRectIsNull(dirty) && !CGRectIsEmpty(dirty)) {
    [self setNeedsDisplayInRect:dirty];
  }
}

- (void)drawRect:(CGRect)rect {
  if (_segment == nil || _rowOffsets.empty()) {
    return;
  }
  const CGSize size = _segment.tableSize;
  const NSStringDrawingOptions options = NSStringDrawingUsesLineFragmentOrigin | NSStringDrawingUsesFontLeading;
  // A theme's border width is in points, and a hairline is the default: one
  // physical pixel, so a grid stays one line on every screen density.
  const CGFloat hairline = _segment.borderWidth > 0 ? _segment.borderWidth
                                                     : 1.0 / self.traitCollection.displayScale;

  if (_segment.hasHeader && _segment.rowHeights.count > 0) {
    [[_segment.theme colorForSlot:kSlotTableHeaderBackgroundColor fallback:UIColor.tertiarySystemFillColor] setFill];
    UIRectFill(CGRectMake(0, 0, size.width - hairline, _segment.rowHeights[0].doubleValue));
  }

  // The zebra fill is drawn under the cells, per row rather than per cell, so
  // an empty trailing cell in a short row still gets the row's fill.
  //
  // A row is one reveal unit, so the whole row draws at one alpha: its fill, its
  // cells, and the grid lines under it. A table row is a block, and its cells are
  // laid out by this grid rather than by a text run, so there is nothing finer to
  // fade within one.
  NSUInteger firstDataRow = _segment.hasHeader ? 1 : 0;
  // The pinned strip draws one row, which is the same loop with a smaller bound:
  // one code path draws the cells, so the header and the row it covers cannot
  // disagree about a column's width.
  const NSUInteger lastRow = self.headerOnly ? 1 : _segment.rows.count;
  // Seek to the dirty row rather than walking a long table's settled prefix on
  // every reveal tick or scroll redraw.
  const NSUInteger firstRow = std::lower_bound(_rowOffsets.begin() + 1, _rowOffsets.end(),
                                               CGRectGetMinY(rect)) - _rowOffsets.begin() - 1;
  for (NSUInteger rowIndex = firstRow; rowIndex < MIN(lastRow, _segment.rows.count); rowIndex++) {
    const CGFloat rowTop = _rowOffsets[rowIndex];
    const CGFloat rowBottom = _rowOffsets[rowIndex + 1];
    if (rowTop > CGRectGetMaxY(rect)) {
      break;
    }
    if (rowBottom < CGRectGetMinY(rect)) {
      continue;
    }
    const CGFloat alpha = self.revealProvider == nil
        ? 1
        : [self.revealProvider revealAlphaAtBlock:self.blockOffset + rowIndex
                                            offset:0
                                               now:CACurrentMediaTime()];
    if (alpha <= 0) {
      continue;
    }
    CGContextRef context = UIGraphicsGetCurrentContext();
    CGContextSaveGState(context);
    CGContextSetAlpha(context, alpha);
    if (rowIndex >= firstDataRow) {
      UIColor *stripe = [_segment.theme colorForSlot:(rowIndex - firstDataRow) % 2 == 0
                                                      ? kSlotTableRowEvenBackgroundColor
                                                      : kSlotTableRowOddBackgroundColor
                                               fallback:nil];
      if (stripe != nil) {
        [stripe setFill];
        UIRectFill(CGRectMake(0, rowTop, size.width, rowBottom - rowTop));
      }
    }
    NSArray<DSTableCell *> *row = _segment.rows[rowIndex];
    for (NSUInteger column = 0; column < row.count; column++) {
      const CGRect content = [self contentRectForRow:rowIndex column:column];
      if (CGRectIntersectsRect(content, rect)) {
        [row[column].text drawWithRect:content options:options context:nil];
      }
    }
    CGContextRestoreGState(context);
  }

  // The grid lines. A pinned strip draws only the line under its own row, or the
  // header would appear to have lines running through it.
  const NSUInteger lineRows = self.headerOnly ? MIN(2u, _segment.rowHeights.count) : _segment.rowHeights.count;
  [[_segment.theme colorForSlot:kSlotTableBorderColor fallback:UIColor.separatorColor] setFill];
  const CGFloat y = _rowOffsets[lineRows];
  const CGRect topLine = CGRectMake(0, 0, size.width - hairline, hairline);
  if (CGRectIntersectsRect(topLine, rect)) {
    UIRectFill(topLine);
  }
  auto line = std::lower_bound(_rowOffsets.begin() + 1, _rowOffsets.begin() + lineRows + 1,
                               CGRectGetMinY(rect));
  for (; line != _rowOffsets.begin() + lineRows + 1 && *line - hairline < CGRectGetMaxY(rect); ++line) {
    UIRectFill(CGRectMake(0, *line - hairline, size.width - hairline, hairline));
  }
  CGFloat x = 0;
  UIRectFill(CGRectMake(_segment.rightToLeft ? size.width - hairline : 0, 0, hairline, y));
  for (NSNumber *width in _segment.columnWidths) {
    x += width.doubleValue;
    UIRectFill(CGRectMake(_segment.rightToLeft ? size.width - hairline - x : x, 0, hairline, y));
  }
}

- (void)traitCollectionDidChange:(UITraitCollection *)previousTraitCollection {
  [super traitCollectionDidChange:previousTraitCollection];
  [self setNeedsDisplay];
}

- (NSInteger)linkIndexAtPoint:(CGPoint)point {
  if (_segment == nil) {
    return -1;
  }
  for (NSUInteger row = 0; row < _segment.rows.count; row++) {
    for (NSUInteger column = 0; column < _segment.rows[row].count; column++) {
      const CGRect content = [self contentRectForRow:row column:column];
      if (!CGRectContainsPoint(CGRectInset(content, -kLinkHitSlop, -kLinkHitSlop), point)) {
        continue;
      }
      DSTextKitStack *stack = [[DSTextKitStack alloc] initWithLayoutManager:[NSLayoutManager new]];
      stack.textContainer.size = CGSizeMake(content.size.width, CGFLOAT_MAX);
      [stack.textStorage setAttributedString:_segment.rows[row][column].text];
      [stack.layoutManager ensureLayoutForTextContainer:stack.textContainer];
      return linkIndexInStack(stack.layoutManager, stack.textContainer,
                              CGPointMake(point.x - content.origin.x, point.y - content.origin.y));
    }
  }
  return -1;
}

- (BOOL)isAccessibilityElement {
  return NO;
}

- (NSArray *)accessibilityElements {
  if (_cachedAccessibilityElements != nil || _segment == nil) {
    return _cachedAccessibilityElements ?: @[];
  }
  NSMutableArray<UIAccessibilityElement *> *elements = [NSMutableArray arrayWithCapacity:_segment.rows.count];
  CGFloat y = 0;
  for (NSUInteger row = 0; row < _segment.rows.count; row++) {
    NSMutableArray<NSString *> *cells = [NSMutableArray new];
    BOOL rowHasLinks = NO;
    for (DSTableCell *cell in _segment.rows[row]) {
      [cells addObject:cell.text.string];
      rowHasLinks |= hasLink(cell.text, NSMakeRange(0, cell.text.length));
    }
    const CGFloat height = _segment.rowHeights[row].doubleValue;
    if (!rowHasLinks) {
      UIAccessibilityElement *element = [[UIAccessibilityElement alloc] initWithAccessibilityContainer:self];
      element.accessibilityLabel = [cells componentsJoinedByString:@", "];
      element.accessibilityFrameInContainerSpace = CGRectMake(0, y, _segment.tableSize.width, height);
      element.accessibilityTraits = row == 0 && _segment.hasHeader ? UIAccessibilityTraitHeader
                                                                   : UIAccessibilityTraitStaticText;
      [elements addObject:element];
    } else {
      const NSUInteger rowStart = elements.count;
      for (NSUInteger column = 0; column < _segment.rows[row].count; column++) {
        DSTableCell *cell = _segment.rows[row][column];
        if (cell.text.length == 0) {
          continue;
        }
        const CGRect content = [self contentRectForRow:row column:column];
        DSTextKitStack *stack = [[DSTextKitStack alloc] initWithLayoutManager:[NSLayoutManager new]];
        stack.textContainer.size = CGSizeMake(content.size.width, CGFLOAT_MAX);
        [stack.textStorage setAttributedString:cell.text];
        [stack.layoutManager ensureLayoutForTextContainer:stack.textContainer];
        const BOOL firstCell = elements.count == rowStart;
        NSString *prefix = firstCell ? [NSString stringWithFormat:@"Row %lu", (unsigned long)row + 1] : nil;
        appendAccessibleSlices(elements, cell.text, NSMakeRange(0, cell.text.length), self,
                               stack.layoutManager, stack.textContainer, content.origin, content, prefix,
                               firstCell && row == 0 && _segment.hasHeader ? UIAccessibilityTraitHeader : 0,
                               self.linkDelegate);
      }
    }
    y += height;
  }
  _cachedAccessibilityElements = elements;
  return elements;
}

@end

/// The viewport around a table's grid, and what it hands a nested scroll.
///
/// A capped table scrolls on both axes inside its own frame, which is the one
/// nested vertical scroll a chat has. The rule is the code card's: this view
/// takes a drag only while it can scroll that way, and at an edge it refuses so
/// the chat list's own pan takes it. `gestureRecognizerShouldBegin` is where that
/// is decided, because a `UIScrollView` that refuses still consumes the touch.
@interface DSTableViewport : UIScrollView <UIScrollViewDelegate>
/// True while this table is still streaming, which is what makes a capped table
/// follow its newest row.
@property (nonatomic) BOOL streaming;
/// Called on every scroll, so the pinned header can follow the horizontal offset.
@property (nonatomic, copy, nullable) void (^onScroll)(void);
/// Moves to the newest row after the next layout, unless the reader has scrolled
/// away from the bottom.
- (void)requestPin;
/// Moves to the newest row now, unless the reader has left.
- (void)followNewestRow;
@end

@implementation DSTableViewport {
  /// A pinned table is one whose reader has not scrolled away from its newest
  /// row. A table that starts streaming is pinned again, the way a code card is.
  BOOL _pinned;
  BOOL _pinAfterLayout;
  /// Only a reader's own scroll unpins. A pin this view applied must not count
  /// as the reader leaving, or a streaming table would unpin itself on the first
  /// row that arrived.
  BOOL _touchScrolling;
}

- (instancetype)initWithFrame:(CGRect)frame {
  if (self = [super initWithFrame:frame]) {
    // Its own delegate, because two of the things it has to observe happen inside
    // `UIScrollView` and reach no public entry point: a reader's drag, which does
    // not go through `setContentOffset:`, and its own scroll position, which the
    // pinned header has to follow. Both funnel into the same one call.
    self.delegate = self;
  }
  return self;
}

- (void)setContentOffset:(CGPoint)offset {
  [super setContentOffset:offset];
  // The programmatic half of the same observation the delegate provides.
  [self notifyScrolled];
}

- (void)scrollViewDidScroll:(UIScrollView *)scrollView {
  if (_touchScrolling) {
    // Streamdown's threshold: this close to the bottom is still "following", so a
    // reader who nudges the table does not lose the pin over a few points.
    const CGFloat bottom = self.contentSize.height - self.bounds.size.height;
    _pinned = bottom - self.contentOffset.y <= kTablePinThreshold;
  }
  [self notifyScrolled];
}

- (void)notifyScrolled {
  if (self.onScroll != nil) {
    self.onScroll();
  }
}

- (void)setStreaming:(BOOL)streaming {
  if (streaming == _streaming) {
    return;
  }
  _streaming = streaming;
  // A table that starts arriving follows its newest row, whether or not it is
  // past its cap yet, so it follows from the moment it crosses.
  if (streaming) {
    _pinned = YES;
    [self requestPin];
  }
}

- (void)requestPin {
  _pinAfterLayout = YES;
  [self setNeedsLayout];
}

/// Moves to the newest row now, unless the reader has left. For this card's own
/// layout pass, which has just given the content its new height: a layout asked
/// for from inside a React Native view does not run until the next snapshot.
- (void)followNewestRow {
  _pinAfterLayout = NO;
  if (_pinned) {
    const CGFloat bottom = MAX(0, self.contentSize.height - self.bounds.size.height);
    [self setContentOffset:CGPointMake(self.contentOffset.x, bottom) animated:NO];
  }
}

- (void)gestureRecognizerDidBegin:(UIGestureRecognizer *)gestureRecognizer {
  _touchScrolling = YES;
}

- (void)gestureRecognizerDidEnd:(UIGestureRecognizer *)gestureRecognizer {
  _touchScrolling = NO;
}

- (void)layoutSubviews {
  [super layoutSubviews];
  if (_pinAfterLayout) {
    _pinAfterLayout = NO;
    // Only while the reader has not said otherwise: a reader who scrolled up to
    // read an earlier row keeps that position while the rest of the table lands.
    if (_pinned) {
      const CGFloat bottom = MAX(0, self.contentSize.height - self.bounds.size.height);
      [self setContentOffset:CGPointMake(self.contentOffset.x, bottom) animated:NO];
    }
  }
}

- (BOOL)gestureRecognizerShouldBegin:(UIGestureRecognizer *)recognizer {
  if (![recognizer isKindOfClass:UIPanGestureRecognizer.class]) {
    return [super gestureRecognizerShouldBegin:recognizer];
  }
  const UIPanGestureRecognizer *pan = (UIPanGestureRecognizer *)recognizer;
  const CGPoint velocity = [pan velocityInView:self];
  const CGFloat maximumY = MAX(0, self.contentSize.height - self.bounds.size.height);
  const CGFloat maximumX = MAX(0, self.contentSize.width - self.bounds.size.width);
  // A drag that starts here belongs to this scroll view only while it can scroll
  // that way. Once it cannot, the chat list's own pan takes the drag, which is
  // what stops a long table from trapping a reader in the message.
  if (ABS(velocity.y) >= ABS(velocity.x)) {
    return velocity.y < 0 ? self.contentOffset.y < maximumY : self.contentOffset.y > 0;
  }
  return velocity.x < 0 ? self.contentOffset.x < maximumX : self.contentOffset.x > 0;
}

@end

@implementation DSTableSegmentView {
  NSArray<NSString *> *_icons;
  DSTableSegment *_segment;
  UIView *_toolbar;
  UIStackView *_buttons;
  DSTableViewport *_scrollView;
  DSTableGridView *_grid;
  /// The header row, drawn over the grid and moved with the horizontal offset so
  /// a reader scrolling sideways keeps the column names in front of them.
  ///
  /// A second grid rather than a copied layer: it reads the same segment and
  /// draws the same cells through the same code, so the pinned header and the
  /// row it covers cannot disagree about a column's width or a cell's text.
  DSTableGridView *_header;
  NSArray<NSString *> *_labels;
  /// The labels the current buttons and menu items were built from, so a changed
  /// `translations` prop rebuilds them instead of leaving stale strings up.
  NSArray<NSString *> *_builtLabels;
  /// The whole table layout, which is what decides this table's cap and whether
  /// the reader has lifted it. The same value the measurement used.
  downstream::TableLayout _tableLayout;
  BOOL _needsScrollRestore;
  BOOL _preserveScrollPosition;
  BOOL _forceScrollReset;
  CGFloat _savedScrollPosition;
  /// The document block this table's first row is, which a reveal unit's own block
  /// index is relative to.
  NSInteger _blockOffset;
  /// Where the rows' fading alphas come from. Weak, because the provider is the
  /// Markdown view's and this card is recycled.
  __weak id<DSRevealProvider> _revealProvider;
}

- (instancetype)initWithFrame:(CGRect)frame {
  if (self = [super initWithFrame:frame]) {
    self.opaque = NO;
    self.backgroundColor = UIColor.clearColor;
    self.contentMode = UIViewContentModeRedraw;
    _toolbar = [UIView new];
    _buttons = [UIStackView new];
    _buttons.axis = UILayoutConstraintAxisHorizontal;
    _buttons.spacing = 2;
    [_toolbar addSubview:_buttons];
    _scrollView = [DSTableViewport new];
    _scrollView.directionalLockEnabled = YES;
    _grid = [DSTableGridView new];
    [_scrollView addSubview:_grid];
    // The pinned header is a sibling of the scroll view rather than a subview of
    // it: a scroll view's own subviews scroll with the content, and a header
    // that scrolled away is the thing this phase exists to stop.
    _header = [DSTableGridView new];
    _header.clipsToBounds = YES;
    // The strip takes no touches at all. It covers the row the grid already
    // hit-tests, so letting it through is what makes a tap in the strip press the
    // cell under it rather than nothing.
    _header.userInteractionEnabled = NO;
    __weak __typeof__(self) weakSelf = self;
    _scrollView.onScroll = ^{
      [weakSelf layoutPinnedHeader];
    };
    [self addSubview:_scrollView];
    [self addSubview:_header];
    [self addSubview:_toolbar];
  }
  return self;
}

- (NSArray<NSString *> *)labels {
  static NSArray<NSString *> *fallback;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    fallback = @[
      @"Copy Code", @"Download file", @"Copied", @"Wrap lines", @"Don't wrap lines", @"Show all lines",
      @"Show fewer lines", @"Copy table", @"Copy table as Markdown", @"Copy table as CSV",
      @"Copy table as TSV", @"Download table", @"Download table as CSV", @"Download table as Markdown",
      @"Markdown", @"CSV", @"TSV",
    ];
  });
  return _labels.count >= DSControlLabelCount ? _labels : fallback;
}

- (NSString *)label:(DSControlLabel)index {
  return self.labels[index];
}

- (void)setControlLabels:(NSArray<NSString *> *)labels {
  _labels = [labels copy];
  [self updateButtons];
}

- (void)setControlIcons:(NSArray<NSString *> *)icons {
  _icons = [icons copy];
  [self updateButtons];
}

/// The glyph for one key, or the platform's default for an entry the app did not
/// name.
- (NSString *)icon:(NSString *)key fallback:(NSString *)fallback {
  static NSArray<NSString *> *keys;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    keys = @[
      DSControlIconCopy, DSControlIconCheck, DSControlIconDownload, DSControlIconMaximize,
      DSControlIconClose, DSControlIconWrap, DSControlIconExpand, DSControlIconCollapse,
      DSControlIconTableFullscreen, DSControlIconDetailsChevron,
    ];
  });
  const NSUInteger index = [keys indexOfObject:key];
  NSString *const named = index < _icons.count ? _icons[index] : @"";
  return named.length > 0 ? named : fallback;
}

- (void)setShowsToolbar:(BOOL)showsToolbar {
  if (showsToolbar == _showsToolbar) {
    return;
  }
  _showsToolbar = showsToolbar;
  _toolbar.hidden = !self.showsToolbar;
  [self setNeedsLayout];
}

- (void)setControls:(uint32_t)controls {
  if (controls == _controls) {
    return;
  }
  _controls = controls;
  [self updateButtons];
}

/// The table's own layout: the cap and the reader's per-table expand choice.
///
/// Both arrive from the Markdown view rather than from the segment, because the
/// choice is the view's state and the segment has no access to it. -1 for a
/// segment that is not a table, which is every other segment view.
- (void)setTableLayout:(downstream::TableLayout)tableLayout {
  if (tableLayout == _tableLayout) {
    return;
  }
  _tableLayout = tableLayout;
  [self updateButtons];
  [self setNeedsLayout];
}

/// True when this table is past its cap, which is the only table the expand
/// button can do anything for.
- (BOOL)capped {
  if (_segment == nil || _segment.tableOrdinal < 0) {
    return NO;
  }
  const downstream::TableBlockLayout table = resolveTable(_tableLayout, _segment.tableOrdinal);
  return table.maxHeight > 0 && _segment.tableSize.height > table.maxHeight;
}

- (BOOL)tableExpanded {
  if (_segment == nil || _segment.tableOrdinal < 0) {
    return NO;
  }
  return resolveTable(_tableLayout, _segment.tableOrdinal).expanded;
}

- (CGFloat)tableMaxHeight {
  if (_segment == nil || _segment.tableOrdinal < 0) {
    return 0;
  }
  return resolveTable(_tableLayout, _segment.tableOrdinal).maxHeight;
}

- (void)setControlsEnabled:(BOOL)enabled {
  if (enabled == _controlsEnabled) {
    return;
  }
  _controlsEnabled = enabled;
  [self updateButtons];
}

/// The formats a table can be copied or saved in. Copy offers every format the
/// engine serializes; save offers the two the file dialog names well, and both
/// platforms offer exactly these, so a reader sees one menu on either.
- (NSArray<NSArray<NSString *> *> *)menuEntriesForAction:(NSString *)action {
  if ([action isEqualToString:@"copy"]) {
    return @[
      @[ [self label:DSControlLabelTableFormatMarkdown], @"markdown" ],
      @[ [self label:DSControlLabelTableFormatCsv], @"csv" ],
      @[ [self label:DSControlLabelTableFormatTsv], @"tsv" ],
    ];
  }
  return @[
    @[ [self label:DSControlLabelTableFormatCsv], @"csv" ],
    @[ [self label:DSControlLabelTableFormatMarkdown], @"markdown" ],
  ];
}

/// A menu button: a glyph that opens a native menu of the formats it can
/// produce. Built once, because a table's controls do not change shape.
- (UIButton *)menuButtonWithSymbol:(NSString *)symbol label:(NSString *)label action:(NSString *)action {
  UIButton *button = makeControlButton(symbol, label, action, @"");
  NSMutableArray<UIMenuElement *> *children = [NSMutableArray array];
  for (NSArray<NSString *> *entry in [self menuEntriesForAction:action]) {
    NSString *format = entry[1];
    [children addObject:[UIAction actionWithTitle:entry[0]
                                           image:nil
                                      identifier:nil
                                         handler:^(__kindof UIAction *_Nonnull inner) {
                                           [self pressFormat:format action:action];
                                         }]];
  }
  button.menu = [UIMenu menuWithTitle:@"" children:children];
  button.showsMenuAsPrimaryAction = YES;
  return button;
}

- (void)buildButtonsIfNeeded {
  if (_buttons.arrangedSubviews.count > 0 && [_builtLabels isEqualToArray:_labels]) {
    return;
  }
  for (UIView *view in [_buttons.arrangedSubviews copy]) {
    [_buttons removeArrangedSubview:view];
    [view removeFromSuperview];
  }
  _builtLabels = [_labels copy];
  UIButton *copy = [self menuButtonWithSymbol:kSymbolCopy
                                        label:[self label:DSControlLabelCopyTable]
                                       action:@"copy"];
  UIButton *download = [self menuButtonWithSymbol:kSymbolDownload
                                            label:[self label:DSControlLabelDownloadTable]
                                           action:@"download"];
  // Expand appears only on a table past the cap, which is the only table it can
  // do anything for, and full screen is offered always: a reader who cannot see
  // the whole table can still leave the chat with it.
  UIButton *expand = makeControlButton(
      [self icon:DSControlIconExpand fallback:symbolForExpand(NO)], [self label:DSControlLabelExpandTable],
      @"expand", @"");
  UIButton *fullscreen = makeControlButton([self icon:DSControlIconTableFullscreen fallback:kSymbolMaximize],
                                          [self label:DSControlLabelViewFullscreen], @"fullscreen", @"");
  copy.tag = 0;
  download.tag = 1;
  expand.tag = 2;
  fullscreen.tag = 3;
  [_buttons addArrangedSubview:copy];
  [_buttons addArrangedSubview:download];
  [_buttons addArrangedSubview:expand];
  [_buttons addArrangedSubview:fullscreen];
  for (UIButton *button in @[copy, download, expand, fullscreen]) {
    button.accessibilityIdentifier = [NSString stringWithFormat:@"ds.table.%@", button.accessibilityLabel];
    [button addTarget:self
                  action:@selector(controlPressed:)
        forControlEvents:UIControlEventTouchUpInside];
  }
}

- (void)controlPressed:(UIButton *)button {
  if (_segment == nil) {
    return;
  }
  // Built as the subclass, so the cast holds: a plain UIButton has no action.
  if ([((DSControlButton *)button).action isEqualToString:@"expand"]) {
    [self.controlDelegate didToggleTableExpandAtOrdinal:static_cast<uint32_t>(_segment.tableOrdinal)
                                                expanded:!self.tableExpanded];
    return;
  }
  // The Markdown is the view's to build: it holds the document, and the segment
  // holds only what was drawn. The export is the same one a copy produces, so a
  // full-screen table is the table rather than a rebuild of it.
  [self.controlDelegate didRequestTableFullscreenAtOrdinal:static_cast<uint32_t>(_segment.tableOrdinal)];
}

- (void)updateButtons {
  if (!self.showsToolbar) {
    _buttons.hidden = YES;
    return;
  }
  _buttons.hidden = NO;
  [self buildButtonsIfNeeded];
  NSArray<UIButton *> *all = _buttons.arrangedSubviews;
  ((UIButton *)all[0]).hidden = (_controls & TableCopy) == 0;
  ((UIButton *)all[1]).hidden = (_controls & TableDownload) == 0;
  // Expand appears only on a table past the cap and only while there is a cap to
  // lift, which is the same rule the code card's expand button follows.
  UIButton *expand = (UIButton *)all[2];
  expand.hidden = (_controls & TableExpand) == 0 || !(self.capped || self.tableExpanded);
  ((UIButton *)all[3]).hidden = (_controls & TableFullscreen) == 0;
  for (UIButton *button in all) {
    button.enabled = _controlsEnabled && !button.hidden;
  }
  // The glyph follows the state, because the two buttons are the same button.
  [expand setImage:controlImage([self icon:(self.tableExpanded ? DSControlIconCollapse : DSControlIconExpand)
                                    fallback:symbolForExpand(self.tableExpanded)])
            forState:UIControlStateNormal];
  expand.accessibilityLabel = self.tableExpanded ? [self label:DSControlLabelCollapseTable]
                                                 : [self label:DSControlLabelExpandTable];
}

- (void)pressFormat:(NSString *)format action:(NSString *)action {
  if (_segment == nil) {
    return;
  }
  if ([action isEqualToString:@"copy"]) {
    [self.controlDelegate didPressControlOfKind:DSBlockKindTableRow
                                   blockIndex:_segment.blockIndex
                                   blockCount:_segment.blockCount
                                     language:@""
                                      action:@"copy"
                                      format:format
                                    filename:@""];
    return;
  }
  [self.controlDelegate didPressControlOfKind:DSBlockKindTableRow
                                   blockIndex:_segment.blockIndex
                                   blockCount:_segment.blockCount
                                     language:@""
                                      action:@"download"
                                      format:format
                                    filename:@""];
}

- (void)setLinkDelegate:(id<DSLinkDelegate>)linkDelegate {
  _linkDelegate = linkDelegate;
  _grid.linkDelegate = linkDelegate;
}

- (void)setSegment:(DSTableSegment *)segment {
  if (segment == _segment) {
    return;
  }
  _preserveScrollPosition = !_forceScrollReset && _segment != nil && segment != nil &&
      _segment.tableIdentifier == segment.tableIdentifier &&
      _segment.rightToLeft == segment.rightToLeft &&
      [_segment.firstRowText isEqualToString:segment.firstRowText];
  const CGFloat oldMaximum = std::max<CGFloat>(0, _scrollView.contentSize.width - _scrollView.bounds.size.width);
  _savedScrollPosition = _segment.rightToLeft
      ? oldMaximum - _scrollView.contentOffset.x
      : _scrollView.contentOffset.x;
  _needsScrollRestore = YES;
  _segment = segment;
  _grid.segment = segment;
  _grid.revealProvider = _revealProvider;
  _grid.blockOffset = _blockOffset;
  // The strip reads the same segment and draws one row out of it, set here rather
  // than on every scroll: the content changes when the table does, and a scroll
  // does not change what the header says.
  _header.segment = segment;
  _header.headerOnly = YES;
  _header.revealProvider = _revealProvider;
  _header.blockOffset = _blockOffset;
  [_header setNeedsDisplay];
  // The toolbar's buttons take the theme's own toolbar color when it set one.
  UIColor *toolbar = segment.toolbarButtonColor ?: UIColor.secondaryLabelColor;
  for (UIView *button in _buttons.arrangedSubviews) {
    [(UIControl *)button setTintColor:toolbar];
  }
  [self updateButtons];
  [self setNeedsLayout];
  [self setNeedsDisplay];
}

- (void)resetScrollPosition {
  _forceScrollReset = YES;
  _preserveScrollPosition = NO;
  _needsScrollRestore = YES;
  [self setNeedsLayout];
}

/// The document block this table's first row is.
- (void)setBlockOffset:(NSInteger)blockOffset {
  if (_blockOffset == blockOffset) {
    return;
  }
  _blockOffset = blockOffset;
  _grid.blockOffset = blockOffset;
  _header.blockOffset = blockOffset;
  [_grid setNeedsDisplay];
  [_header setNeedsDisplay];
}

/// Each block is one row. Only that row's visible pixels change during reveal;
/// the pinned header has its own backing store and needs the same invalidation.
- (void)invalidateRevealCharacterRange:(NSRange)range
                            blockOffset:(NSInteger)blockOffset
                                     now:(double)now {
  (void)range;
  (void)now;
  if (blockOffset < _blockOffset || static_cast<NSUInteger>(blockOffset - _blockOffset) >= _segment.rows.count) {
    return;
  }
  const NSUInteger row = static_cast<NSUInteger>(blockOffset - _blockOffset);
  [_grid invalidateRevealRow:row visibleRect:[_grid convertRect:_scrollView.bounds fromView:_scrollView]];
  if (row == 0 && !_header.hidden) {
    [_header invalidateRevealRow:row visibleRect:_header.bounds];
  }
}

- (void)setRevealProvider:(id<DSRevealProvider>)provider {
  if (_revealProvider == provider) {
    return;
  }
  _revealProvider = provider;
  _grid.revealProvider = provider;
  _header.revealProvider = provider;
  [_grid setNeedsDisplay];
  [_header setNeedsDisplay];
}

- (void)layoutSubviews {
  [super layoutSubviews];
  if (_segment == nil) {
    return;
  }
  const CGFloat toolbarHeight = self.showsToolbar ? kChromeRowHeight : 0;
  // A toolbar above the grid takes layout space rather than floating over the
  // rows, so a reader never taps a button instead of a cell.
  _toolbar.frame = CGRectMake(0, 0, self.bounds.size.width, toolbarHeight);
  // The buttons keep their own width at the trailing edge, as the code header's
  // do; a row-wide stack would stretch them apart.
  const CGFloat buttonsWidth =
      [_buttons systemLayoutSizeFittingSize:UILayoutFittingCompressedSize].width;
  const BOOL rightToLeft =
      self.effectiveUserInterfaceLayoutDirection == UIUserInterfaceLayoutDirectionRightToLeft;
  _buttons.frame = CGRectMake(rightToLeft ? 4 : self.bounds.size.width - buttonsWidth - 4, 0, buttonsWidth,
                              toolbarHeight);
  const CGFloat leading = _segment.contentLeading;
  // The pinned header takes a row of the card above the scroll view rather than
  // floating over the first one, so a reader sees one header and never two, and a
  // tap in that strip never lands on a row instead of the header.
  const CGFloat headerHeight = self.headerRowHeight;
  _scrollView.frame = CGRectMake(_segment.rightToLeft ? 0 : leading, toolbarHeight + headerHeight,
                                 std::max<CGFloat>(0, self.bounds.size.width - leading),
                                 std::max<CGFloat>(0, self.bounds.size.height - toolbarHeight - headerHeight));
  _grid.frame = CGRectMake(0, -headerHeight, _segment.tableSize.width, _segment.tableSize.height);
  _scrollView.contentSize = CGSizeMake(_segment.tableSize.width,
      std::max<CGFloat>(0, _segment.tableSize.height - headerHeight));
  // The grid scrolls on both axes only while it is capped. An uncapped table has
  // nothing to scroll vertically, and a scroll view that could would take a
  // reader's drag away from the chat for no reason.
  const BOOL capped = self.capped;
  _scrollView.scrollEnabled = capped || _segment.tableSize.width > _scrollView.bounds.size.width;
  _scrollView.alwaysBounceVertical = capped;
  _scrollView.showsVerticalScrollIndicator = capped;
  _scrollView.streaming = _streaming;
  if (_needsScrollRestore) {
    const CGFloat maximum = std::max<CGFloat>(0, _segment.tableSize.width - _scrollView.bounds.size.width);
    const CGFloat position = _preserveScrollPosition ? _savedScrollPosition : 0;
    _scrollView.contentOffset = CGPointMake(_segment.rightToLeft
        ? std::clamp(maximum - position, 0.0, maximum)
        : std::clamp(position, 0.0, maximum), 0);
    _needsScrollRestore = NO;
    _forceScrollReset = NO;
  }
  [self layoutPinnedHeader];
  if (capped && _streaming) {
    // Follow the newest row, and not a snapshot: the pin has to land after the
    // content has its new height, which the layout above has now given it.
    [_scrollView followNewestRow];
  }
}

/// The header row's height, or 0 for a table with no header.
///
/// A table without a header row draws none and collapses the strip, so a header
/// is never space a reader pays for and does not get.
- (CGFloat)headerRowHeight {
  if (_segment == nil || !_segment.hasHeader || _segment.rowHeights.count == 0) {
    return 0;
  }
  return _segment.rowHeights[0].doubleValue;
}

/// The header row, pinned above the grid and moved with the horizontal offset.
///
/// A table is the one block where a column means nothing without its name, so a
/// reader who scrolls sideways to see a column has to keep that name in front of
/// them. The pinned strip is a sibling of the scroll view, because a scroll
/// view's own subviews move with the content.
///
/// It is drawn by a second grid reading the same segment, not by a copied layer:
/// one code path draws the cells, so the pinned header and the row it covers
/// cannot disagree about a column's width or a cell's text.
///
/// A table with no header draws nothing and the strip collapses, so it never
/// covers a row.
- (void)layoutPinnedHeader {
  const CGFloat headerHeight = self.headerRowHeight;
  if (headerHeight <= 0) {
    _header.hidden = YES;
    _header.frame = CGRectZero;
    return;
  }
  _header.hidden = NO;
  // Above the scroll view, which is what makes it pinned, and offset sideways by
  // whatever the grid has scrolled to so the strip and the row it covers stay in
  // the same columns. Only the frame moves here: the strip draws the same content
  // whatever the offset is, so a scroll costs one assignment rather than a
  // redraw.
  _header.frame = CGRectMake(_scrollView.frame.origin.x, _scrollView.frame.origin.y - headerHeight,
                             _scrollView.bounds.size.width, headerHeight);
  _header.bounds = CGRectMake(_scrollView.contentOffset.x, 0, _header.frame.size.width, headerHeight);
}

// Quote bars and the list marker live in the non-scrolling margin.
- (void)drawRect:(CGRect)rect {
  if (_segment == nil) {
    return;
  }
  const CGFloat toolbarHeight = self.showsToolbar ? kChromeRowHeight : 0;
  for (DSContainerInfo *container in _segment.containers) {
    if (container.barColor != nil && container.barWidth > 0) {
      [container.barColor setFill];
      // The bar sits two points inside the container, and its width is the
      // theme's, so a theme that sets a wider bar draws a wider bar here and in
      // a plain text block alike.
      const CGFloat x = _segment.rightToLeft
          ? self.bounds.size.width - container.leading - 2 - container.barWidth
          : container.leading + 2;
      UIRectFill(CGRectMake(x, toolbarHeight, container.barWidth, self.bounds.size.height - toolbarHeight));
    }
    if (container.marker.length > 0) {
      UIFont *font = [container.marker attribute:NSFontAttributeName atIndex:0 effectiveRange:NULL];
      const CGSize markerSize = container.marker.size;
      const CGFloat baseline = toolbarHeight + _segment.cellPadding.top + font.ascender;
      const CGFloat x = _segment.rightToLeft
          ? self.bounds.size.width - container.leading - container.width + container.markerGap
          : container.leading + container.width - container.markerGap - markerSize.width;
      [container.marker drawAtPoint:CGPointMake(x, baseline - font.ascender)];
    }
  }
}

- (void)traitCollectionDidChange:(UITraitCollection *)previousTraitCollection {
  [super traitCollectionDidChange:previousTraitCollection];
  [self setNeedsDisplay];
}

- (NSInteger)linkIndexAtPoint:(CGPoint)point {
  return [_grid linkIndexAtPoint:[self convertPoint:point toView:_grid]];
}

/// True while this table is still streaming, which is what makes a capped table
/// follow its newest row.
- (void)setStreaming:(BOOL)streaming {
  if (streaming == _streaming) {
    return;
  }
  _streaming = streaming;
  _scrollView.streaming = streaming;
}

- (BOOL)pointInside:(CGPoint)point withEvent:(UIEvent *)event {
  return CGRectContainsPoint(CGRectInset(self.bounds, -6, 0), point);
}

- (NSArray *)accessibilityElements {
  if (_segment == nil) {
    return @[];
  }
  // The buttons come before the rows, as in Streamdown's DOM.
  NSMutableArray *elements = [NSMutableArray new];
  if (self.showsToolbar) {
    for (UIView *view in _buttons.arrangedSubviews) {
      if (!view.hidden && [view isAccessibilityElement]) {
        [elements addObject:view];
      }
    }
  }
  // The grid is listed as a container, not flattened: its rows' frames are in
  // the grid's space, which the toolbar and the horizontal scroll move.
  [elements addObject:_grid];
  return elements;
}

@end
