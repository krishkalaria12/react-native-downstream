#import "DSSegmentViews.h"

#import "DSRenderedDocument.h"

#include "Controls.h"
#include "Reveal.h"

#include <algorithm>
#include <utility>

using namespace downstream;

const CGFloat kLinkHitSlop = 4;

BOOL hasLink(NSAttributedString *text, NSRange range) {
  __block BOOL found = NO;
  [text enumerateAttribute:DSLinkIndexAttributeName
                   inRange:range
                   options:0
                usingBlock:^(NSNumber *link, NSRange effectiveRange, BOOL *stop) {
                  if (link != nil) {
                    found = YES;
                    *stop = YES;
                  }
                }];
  return found;
}

/// Link index under a point in a laid-out TextKit stack, or -1.
NSInteger linkIndexInStack(NSLayoutManager *layoutManager, NSTextContainer *container, CGPoint location) {
  if (layoutManager.numberOfGlyphs == 0) {
    return -1;
  }
  const NSUInteger glyph = [layoutManager glyphIndexForPoint:location
                                             inTextContainer:container
                              fractionOfDistanceThroughGlyph:NULL];
  if (glyph >= layoutManager.numberOfGlyphs) {
    return -1;
  }
  const CGRect glyphRect = [layoutManager boundingRectForGlyphRange:NSMakeRange(glyph, 1) inTextContainer:container];
  if (!CGRectContainsPoint(CGRectInset(glyphRect, -kLinkHitSlop, -kLinkHitSlop), location)) {
    return -1;
  }
  const NSUInteger character = [layoutManager characterIndexForGlyphAtIndex:glyph];
  NSNumber *link = [layoutManager.textStorage attribute:DSLinkIndexAttributeName atIndex:character effectiveRange:NULL];
  return link != nil ? link.integerValue : -1;
}

@interface DSLinkAccessibilityElement : UIAccessibilityElement
@property (nonatomic, weak, nullable) id<DSLinkDelegate> linkDelegate;
@property (nonatomic) NSInteger linkIndex;
@end

@implementation DSLinkAccessibilityElement

- (BOOL)accessibilityActivate {
  id<DSLinkDelegate> delegate = self.linkDelegate;
  if (delegate == nil) {
    return NO;
  }
  [delegate didPressLinkAtIndex:self.linkIndex];
  return YES;
}

@end

void invalidateAccessibleLinks(NSArray<UIAccessibilityElement *> *elements) {
  for (UIAccessibilityElement *element in elements) {
    if ([element isKindOfClass:DSLinkAccessibilityElement.class]) {
      ((DSLinkAccessibilityElement *)element).linkDelegate = nil;
    }
  }
}

/// Split only ranges containing links. Each link gets a focusable element in
/// source order; plain text remains readable without repeating link labels.
void appendAccessibleSlices(NSMutableArray<UIAccessibilityElement *> *elements, NSAttributedString *text,
                            NSRange range, UIView *container, NSLayoutManager *layoutManager,
                            NSTextContainer *textContainer, CGPoint origin, CGRect fallbackFrame,
                            NSString *_Nullable prefix, UIAccessibilityTraits firstTraits,
                            id<DSLinkDelegate> _Nullable delegate) {
  NSUInteger cursor = range.location;
  const NSUInteger end = NSMaxRange(range);
  BOOL first = YES;
  while (cursor < end) {
    NSRange slice;
    NSNumber *link = [text attribute:DSLinkIndexAttributeName
                           atIndex:cursor
             longestEffectiveRange:&slice
                           inRange:range];
    if (slice.length == 0) {
      break;
    }
    UIAccessibilityElement *element;
    if (link != nil) {
      DSLinkAccessibilityElement *linkElement =
          [[DSLinkAccessibilityElement alloc] initWithAccessibilityContainer:container];
      linkElement.linkDelegate = delegate;
      linkElement.linkIndex = link.integerValue;
      element = linkElement;
    } else {
      element = [[UIAccessibilityElement alloc] initWithAccessibilityContainer:container];
    }
    NSString *label = [text.string substringWithRange:slice];
    if (first && prefix.length > 0) {
      label = [NSString stringWithFormat:@"%@, %@", prefix, label];
    }
    element.accessibilityLabel = label;
    element.accessibilityTraits = (link != nil ? UIAccessibilityTraitLink : UIAccessibilityTraitStaticText) |
        (first ? firstTraits : 0);
    const NSRange glyphs = [layoutManager glyphRangeForCharacterRange:slice actualCharacterRange:NULL];
    const CGRect glyphFrame = [layoutManager boundingRectForGlyphRange:glyphs inTextContainer:textContainer];
    element.accessibilityFrameInContainerSpace = CGRectIsEmpty(glyphFrame)
        ? fallbackFrame
        : CGRectOffset(glyphFrame, origin.x, origin.y);
    [elements addObject:element];
    first = NO;
    cursor = NSMaxRange(slice);
  }
}

#pragma mark - Control buttons

/// 28 points of glyph in a 44-point target, which is the smallest comfortable
/// touch target on the platform.
constexpr CGFloat kButtonGlyph = 28;
/// The height of a control row, matching the one the measurement reserves.
const CGFloat kChromeRowHeight = 32;

NSString *const kSymbolCopy = @"doc.on.doc";
NSString *const kSymbolCheck = @"checkmark";
NSString *const kSymbolDownload = @"arrow.down.to.line";

/// Wrapping is a state, so it is drawn as one: a glyph with an arrow through
/// it when the block wraps and a plain one when it does not. SF Symbols has no
/// wrap glyph, so the state is read from the button's selected trait and the
/// arrow is drawn rather than looked up.
NSString *const kSymbolWrap = @"text.alignleft";
NSString *const kSymbolUnwrap = @"text.alignleft";

/// Expanding is a state, so it is drawn as one: the two arrows point apart when
/// the block grows past the cap and together when the cap comes back.
NSString *const kSymbolExpand = @"arrow.up.left.and.arrow.down.right";
NSString *const kSymbolCollapse = @"arrow.down.left.and.arrow.up.right";

/// Phase 2: a table's full-screen button, and the chevron a `<details>` summary
/// opens and closes with. Both are Downstream's own names, because Streamdown's
/// full-screen table is a browser viewport and its chevron belongs to the
/// browser's own widget rather than to this library.
NSString *const kSymbolMaximize = @"arrow.up.left.and.arrow.down.right";
NSString *const kSymbolChevronDown = @"chevron.down";
NSString *const kSymbolChevronRight = @"chevron.right";

@implementation DSControlButton
@end

/// Every control glyph at one size, so a glyph swapped in later (a copy's check,
/// a wrap state) matches the ones built with the button.
UIImage *controlImage(NSString *symbol) {
  UIImageSymbolConfiguration *configuration =
      [UIImageSymbolConfiguration configurationWithPointSize:kButtonGlyph * 0.5
                                                      weight:UIImageSymbolWeightRegular];
  return [UIImage systemImageNamed:symbol withConfiguration:configuration];
}

UIButton *makeControlButton(NSString *symbol, NSString *label, NSString *action, NSString *format) {
  // Built as the subclass, not cast to it: a plain UIButton has no action or
  // format, and setting either would throw.
  DSControlButton *button = [DSControlButton buttonWithType:UIButtonTypeSystem];
  [button setImage:controlImage(symbol) forState:UIControlStateNormal];
  [button setTintColor:UIColor.secondaryLabelColor];
  button.accessibilityLabel = label;
  button.frame = CGRectMake(0, 0, kButtonGlyph, kButtonGlyph);
  // A fixed width, so a swapped glyph (copy's check) does not shift the others.
  [button.widthAnchor constraintEqualToConstant:kButtonGlyph].active = YES;
  button.action = action;
  button.format = format;
  return button;
}

NSString *symbolForCopy(BOOL copied) {
  return copied ? kSymbolCheck : kSymbolCopy;
}

NSString *symbolForWrap(BOOL wrapped) {
  return wrapped ? kSymbolUnwrap : kSymbolWrap;
}

NSString *symbolForExpand(BOOL expanded) {
  return expanded ? kSymbolCollapse : kSymbolExpand;
}

#pragma mark - Document

@implementation DSDocumentView {
  DSRenderedDocument *_rendered;
  NSMutableArray<UIView<DSSegmentView> *> *_segmentViews;
  NSArray<NSString *> *_labels;
  uint32_t _chrome;
  /// A code block's ordinals, so a wrap or expand choice can be applied by
  /// ordinal rather than by segment index, which moves as blocks stream in.
  NSMutableArray<NSNumber *> *_codeOrdinals;
  /// The whole code layout, which both the card views and the frame stack read,
  /// so a block's drawn layout and its measured height are the same decision.
  downstream::CodeLayout _codeLayout;
  /// The whole table layout, which the table segments draw from and the frame
  /// stack is keyed by, for the same reason `codeLayout` is.
  downstream::TableLayout _tableLayout;
  /// The reader's one set of choices. A text segment's chevron reads it directly
  /// rather than through either layout, because a collapsible block is neither a
  /// code block's nor a table's decision.
  downstream::ReaderChoices _detailsChoices;
  /// What each image is, which the image views draw from and the frame stack is
  /// keyed by. A load changes an entry, which re-stacks the frames.
  downstream::ImageStateLayout _imageState;
  /// Which fenced blocks a React child replaced, and how tall each one is. A
  /// code segment whose ordinal is in here draws as a host segment: its
  /// container chrome and nothing else, with the child drawn above it.
  downstream::HostLayout _hostLayout;
  /// Whether the document's blocks are all settled, which turns an image
  /// block's controls on and stops a pending box from being a control's target.
  BOOL _settled;
  /// The screen scale pictures are decoded at, so a 4,000-pixel photo costs a
  /// phone-sized bitmap.
  CGFloat _screenScale;
  /// The theme's selection color, applied to every text view in the stack.
  UIColor *_selectionColor;
  /// Where this stack's fading characters are. Weak, because the layout manager is
  /// owned by a view this one owns and a fade that outlived its provider would
  /// draw into nothing.
  __weak id<DSRevealProvider> _revealProvider;
}

- (instancetype)initWithFrame:(CGRect)frame {
  if (self = [super initWithFrame:frame]) {
    self.backgroundColor = UIColor.clearColor;
    _segmentViews = [NSMutableArray new];
  }
  return self;
}

/// The view class a segment is drawn by, so a recycled view of the wrong kind is
/// replaced rather than reconfigured into something it cannot be.
///
/// The kind is compared rather than the class, because a segment's class is what
/// a recycled view's `isKindOfClass:` answers to and a class comparison would say
/// "not reusable" for a subclass a future release introduces.
static Class segmentViewClassFor(id segment) {
  if ([segment isKindOfClass:DSTextSegment.class]) {
    return DSTextSegmentView.class;
  }
  if ([segment isKindOfClass:DSCodeSegment.class]) {
    return DSCodeSegmentView.class;
  }
  if ([segment isKindOfClass:DSMathSegment.class]) {
    return DSMathSegmentView.class;
  }
  if ([segment isKindOfClass:DSImageSegment.class]) {
    return DSImageSegmentView.class;
  }
  return DSTableSegmentView.class;
}

/// A segment's controls and its selection turn on once every block it covers
/// is settled. A streaming tail is rebuilt on every snapshot and its selection
/// cleared, so a selection there would vanish mid-drag; earlier text is settled
/// and keeps its selection.
- (BOOL)isSegmentSettledAtIndex:(NSUInteger)index {
  if (_rendered == nil || index >= _segmentViews.count) {
    return NO;
  }
  const NSArray<NSNumber *> *indexes = _rendered.segmentBlockIndexes;
  if (indexes.count <= index) {
    return NO;
  }
  return [indexes[index] unsignedIntegerValue] + 1 <= _rendered.settledBlocks;
}

/// Whether the code segment at `index` is drawn by a React child.
///
/// Asked through the segment's own ordinal rather than its index, because an
/// ordinal names the block across every snapshot and an index moves as blocks
/// stream in.
- (BOOL)hostsCodeOrdinalAtIndex:(NSUInteger)index {
  if (index >= _codeOrdinals.count) {
    return NO;
  }
  const uint32_t ordinal = (uint32_t)_codeOrdinals[index].unsignedIntValue;
  if (ordinal == UINT32_MAX) {
    return NO;
  }
  return _hostLayout.hosts(ordinal);
}

- (void)applyStateToSegmentView:(UIView<DSSegmentView> *)view atIndex:(NSUInteger)index {
  if ([view isKindOfClass:DSTextSegmentView.class]) {
    DSTextSegmentView *text = (DSTextSegmentView *)view;
    text.selectable = _selectable && [self isSegmentSettledAtIndex:index];
    // The chevron is the control's own glyph, so it appears with the control and
    // points the way the body will move.
    text.detailsToggle = (_controls & downstream::ControlBits::DetailsToggle) != 0;
    text.detailsChoices = _detailsChoices;
    text.controlDelegate = _controlDelegate;
    return;
  }
  if ([view isKindOfClass:DSCodeSegmentView.class]) {
    DSCodeSegmentView *code = (DSCodeSegmentView *)view;
    // Whether this block is a host segment comes from the state, not from the
    // segment: the segment is the same card either way, and only the view knows
    // whether a React child mounted for it.
    code.hosted = [self hostsCodeOrdinalAtIndex:index];
    code.showsHeader = (_chrome & downstream::kChromeCodeHeader) != 0;
    code.controls = _controls;
    code.controlsEnabled = [self isSegmentSettledAtIndex:index];
    // A block that is settled keeps its offset and stops following its newest
    // line. `Document::settledBlocks` is the same fact pinning reads, so a card
    // and the parser cannot disagree about whether the fence has closed.
    code.streaming = ![self isSegmentSettledAtIndex:index];
    if (_codeOrdinals.count > index) {
      // The same two calls the measurement used, so the frame this card is given
      // and the layout it draws inside it are one decision.
      DSCodeSegment *card = code.segment;
      code.layout = downstream::resolveCodeBlock(
          _codeLayout, _codeOrdinals[index].unsignedIntValue, card.fenceMeta);
    }
    return;
  }
  if ([view isKindOfClass:DSImageSegmentView.class]) {
    // An image block's button and its pending state both turn on once the block
    // is settled, which is the same rule a code block's controls follow.
    DSImageSegmentView *pictures = (DSImageSegmentView *)view;
    pictures.controls = _controls;
    pictures.controlsEnabled = [self isSegmentSettledAtIndex:index];
    pictures.settled = [self isSegmentSettledAtIndex:index];
    pictures.screenScale = _screenScale;
    [pictures setImageState:_imageState];
    return;
  }
  if ([view isKindOfClass:DSMathSegmentView.class]) {
    // A formula has no controls and nothing to settle: its layout is the same
    // whether the fence has closed or not, because an open fence's last good
    // layout is what it draws.
    ((DSMathSegmentView *)view).streaming = ![self isSegmentSettledAtIndex:index];
    return;
  }
  DSTableSegmentView *table = (DSTableSegmentView *)view;
  table.showsToolbar = (_chrome & downstream::kChromeTableToolbar) != 0;
  table.controls = _controls;
  table.controlsEnabled = [self isSegmentSettledAtIndex:index];
  // The same settled rule a code card follows, for the same reason: a table whose
  // rows are still arriving follows its newest row, and one that has closed keeps
  // the position the reader left it at.
  table.streaming = ![self isSegmentSettledAtIndex:index];
  table.tableLayout = _tableLayout;
}

/// Where the document's fading characters are. Set on every render because the
/// provider holds the live unit list, which changes on every snapshot.
- (void)setRevealProvider:(id<DSRevealProvider>)provider {
  if (_revealProvider == provider) {
    return;
  }
  _revealProvider = provider;
  for (UIView<DSSegmentView> *view in _segmentViews) {
    if ([view isKindOfClass:DSTextSegmentView.class]) {
      ((DSTextSegmentView *)view).revealProvider = provider;
    } else if ([view isKindOfClass:DSCodeSegmentView.class]) {
      ((DSCodeSegmentView *)view).revealProvider = provider;
    } else if ([view isKindOfClass:DSTableSegmentView.class]) {
      ((DSTableSegmentView *)view).revealProvider = provider;
    }
  }
}

- (void)setRendered:(DSRenderedDocument *)rendered {
  if (rendered == _rendered) {
    return;
  }
  _rendered = rendered;
  NSArray *segments = rendered.segments ?: @[];
  // Ordinals first: a code card's wrap state is looked up by ordinal, and the
  // view loop below is where the cards are created.
  NSMutableArray<NSNumber *> *ordinals = [NSMutableArray arrayWithCapacity:segments.count];
  for (id segment in segments) {
    [ordinals addObject:[segment isKindOfClass:DSCodeSegment.class]
                            ? @(((DSCodeSegment *)segment).codeOrdinal)
                            : @(-1)];
  }
  _codeOrdinals = ordinals;
  for (NSUInteger index = 0; index < segments.count; index++) {
    id segment = segments[index];
    UIView<DSSegmentView> *view = index < _segmentViews.count ? _segmentViews[index] : nil;
    const BOOL reusable = view != nil && [view isKindOfClass:segmentViewClassFor(segment)];
    if (!reusable) {
      [view removeFromSuperview];
      if ([segment isKindOfClass:DSTextSegment.class]) {
        DSTextSegmentView *textView = [DSTextSegmentView new];
        textView.linkDelegate = _linkDelegate;
        view = textView;
      } else if ([segment isKindOfClass:DSCodeSegment.class]) {
        DSCodeSegmentView *codeView = [DSCodeSegmentView new];
        codeView.linkDelegate = _linkDelegate;
        codeView.controlDelegate = self.controlDelegate;
        [codeView setControlLabels:_labels];
        view = codeView;
      } else if ([segment isKindOfClass:DSMathSegment.class]) {
        // A formula has no controls and no links, so it is given neither
        // delegate: a button that does nothing is worse than no button.
        view = [DSMathSegmentView new];
      } else if ([segment isKindOfClass:DSImageSegment.class]) {
        DSImageSegmentView *imageView = [DSImageSegmentView new];
        imageView.linkDelegate = _linkDelegate;
        imageView.imageDelegate = _imageDelegate;
        imageView.controlDelegate = self.controlDelegate;
        [imageView setControlLabels:_labels];
        view = imageView;
      } else {
        DSTableSegmentView *tableView = [DSTableSegmentView new];
        tableView.linkDelegate = _linkDelegate;
        tableView.controlDelegate = self.controlDelegate;
        [tableView setControlLabels:_labels];
        view = tableView;
      }
      if (index < _segmentViews.count) {
        _segmentViews[index] = view;
      } else {
        [_segmentViews addObject:view];
      }
      [self addSubview:view];
    }
    if ([segment isKindOfClass:DSTextSegment.class]) {
      [(DSTextSegmentView *)view setSegment:segment];
    } else if ([segment isKindOfClass:DSCodeSegment.class]) {
      [(DSCodeSegmentView *)view setSegment:segment];
    } else if ([segment isKindOfClass:DSMathSegment.class]) {
      [(DSMathSegmentView *)view setSegment:segment];
    } else if ([segment isKindOfClass:DSImageSegment.class]) {
      DSImageSegmentView *imageView = (DSImageSegmentView *)view;
      imageView.imageDelegate = _imageDelegate;
      [imageView setSegment:segment];
    } else {
      [(DSTableSegmentView *)view setSegment:segment];
    }
    // A reveal unit names a document block, so every view that draws text needs
    // to know which document block its own first character is. Set on every
    // render rather than once, because a view is reused across snapshots and the
    // segment it holds is a different range each time. The offset is set even
    // with no provider, because one can arrive after the segments do.
    {
      NSNumber *from = index < _rendered.segmentBlockIndexes.count
          ? _rendered.segmentBlockIndexes[index]
          : nil;
      if ([view isKindOfClass:DSTextSegmentView.class]) {
        DSTextSegmentView *text = (DSTextSegmentView *)view;
        text.blockOffset = from.integerValue;
        text.revealProvider = _revealProvider;
      } else if ([view isKindOfClass:DSCodeSegmentView.class]) {
        DSCodeSegmentView *code = (DSCodeSegmentView *)view;
        code.blockOffset = from.integerValue;
        code.revealProvider = _revealProvider;
      } else if ([view isKindOfClass:DSTableSegmentView.class]) {
        DSTableSegmentView *table = (DSTableSegmentView *)view;
        table.blockOffset = from.integerValue;
        table.revealProvider = _revealProvider;
      }
    }
    // A view created here has not seen the selection color yet, and a theme can
    // arrive after the segments do.
    if (_selectionColor != nil) {
      if ([view isKindOfClass:DSTextSegmentView.class]) {
        ((DSTextSegmentView *)view).selectionColor = _selectionColor;
      } else if ([view isKindOfClass:DSCodeSegmentView.class]) {
        ((DSCodeSegmentView *)view).selectionColor = _selectionColor;
      }
    }
    [self applyStateToSegmentView:view atIndex:index];
  }
  while (_segmentViews.count > segments.count) {
    [_segmentViews.lastObject removeFromSuperview];
    [_segmentViews removeLastObject];
  }
  while (_codeOrdinals.count > segments.count) {
    [_codeOrdinals removeLastObject];
  }
  [self setNeedsLayout];
}

/// Segment starts are in document order. Find the one segment owning the block
/// without inspecting every earlier view on each display-link tick.
- (UIView<DSRevealInvalidatable> *)revealViewForBlock:(NSInteger)block {
  if (block < 0) {
    return nil;
  }
  NSArray<NSNumber *> *indexes = _rendered.segmentBlockIndexes;
  NSUInteger low = 0;
  NSUInteger high = std::min(indexes.count, _segmentViews.count);
  while (low < high) {
    const NSUInteger middle = low + (high - low) / 2;
    if (indexes[middle].integerValue <= block) {
      low = middle + 1;
    } else {
      high = middle;
    }
  }
  if (low == 0) {
    return nil;
  }
  const NSUInteger index = low - 1;
  UIView *view = _segmentViews[index];
  if (![view conformsToProtocol:@protocol(DSRevealInvalidatable)]) {
    return nil;
  }
  id segment = _rendered.segments[index];
  NSInteger count = 1;
  if ([segment isKindOfClass:DSTextSegment.class]) {
    count = (NSInteger)((DSTextSegment *)segment).blockRanges.count;
  } else if ([segment isKindOfClass:DSTableSegment.class]) {
    count = ((DSTableSegment *)segment).blockCount;
  }
  // In particular, do not map a past-end block onto the last segment or a
  // table's later rows onto the segment following the table.
  if (block - indexes[index].integerValue >= count) {
    return nil;
  }
  return (UIView<DSRevealInvalidatable> *)view;
}

- (void)invalidateRevealUnits:(std::span<const downstream::RevealUnit>)units
                    document:(const downstream::Document &)document
                         now:(double)now {
  // RevealTracker stores units in block/start order. All units of a block
  // touch the same segment, so invalidate their union once, without making an
  // array or repeatedly asking TextKit to invalidate the same line fragment.
  size_t index = 0;
  while (index < units.size()) {
    const uint32_t block = units[index].block;
    uint32_t start = units[index].start;
    uint32_t end = units[index].end;
    do {
      start = std::min(start, units[index].start);
      end = std::max(end, units[index].end);
      index++;
    } while (index < units.size() && units[index].block == block);
    if (block >= document.blocks.size()) {
      continue;
    }
    // Text rendering reserves U+200B for an empty rule/fence/paragraph.
    // Its single reveal unit still needs a display invalidation on completion.
    const size_t length = std::max<size_t>(1, document.blocks[block].text.size());
    const NSUInteger from = std::min<size_t>(start, length);
    const NSUInteger to = std::min<size_t>(end, length);
    [[self revealViewForBlock:block] invalidateRevealCharacterRange:NSMakeRange(from, to - from)
                                                       blockOffset:block
                                                                now:now];
  }
}

- (NSArray<UIView<DSRevealInvalidatable> *> *)revealViewsForBlock:(NSInteger)block {
  UIView<DSRevealInvalidatable> *view = [self revealViewForBlock:block];
  return view == nil ? @[] : @[view];
}

/// TextKit draws its own selection, so a themed `colors.selection` has to reach
/// the text views rather than the segments: there is no other place to set it.
- (void)setSelectionColor:(UIColor *)selectionColor {
  _selectionColor = selectionColor;
  for (UIView<DSSegmentView> *view in _segmentViews) {
    if ([view isKindOfClass:DSTextSegmentView.class]) {
      ((DSTextSegmentView *)view).selectionColor = selectionColor;
    } else if ([view isKindOfClass:DSCodeSegmentView.class]) {
      ((DSCodeSegmentView *)view).selectionColor = selectionColor;
    }
  }
  [self setNeedsDisplay];
}

- (void)setSelectable:(BOOL)selectable {
  _selectable = selectable;
  [self reapplyState];
}

- (void)setControls:(uint32_t)controls {
  _controls = controls;
  [self reapplyState];
}

- (void)setHostLayout:(downstream::HostLayout)hostLayout {
  if (_hostLayout == hostLayout) {
    return;
  }
  _hostLayout = std::move(hostLayout);
  for (NSUInteger index = 0; index < _segmentViews.count; index++) {
    [self applyStateToSegmentView:_segmentViews[index] atIndex:index];
  }
  [self setNeedsLayout];
}

- (void)setChrome:(uint32_t)chrome {
  if (chrome == _chrome) {
    return;
  }
  _chrome = chrome;
  [self reapplyState];
}

- (void)setCodeLayout:(downstream::CodeLayout)codeLayout {
  if (codeLayout == _codeLayout) {
    return;
  }
  _codeLayout = codeLayout;
  [self reapplyState];
}

- (void)setTableLayout:(downstream::TableLayout)tableLayout {
  if (tableLayout == _tableLayout) {
    return;
  }
  _tableLayout = tableLayout;
  // The one set of choices rides along with the props it belongs to, so a table
  // prop that did not change still carries a collapsed body to the chevrons.
  _detailsChoices = tableLayout.choices;
  for (UIView<DSSegmentView> *view in _segmentViews) {
    if ([view isKindOfClass:DSTableSegmentView.class]) {
      ((DSTableSegmentView *)view).tableLayout = tableLayout;
    } else if ([view isKindOfClass:DSTextSegmentView.class]) {
      ((DSTextSegmentView *)view).detailsChoices = _detailsChoices;
    }
  }
  [self setNeedsLayout];
}

- (void)setImageState:(downstream::ImageStateLayout)imageState {
  if (imageState == _imageState) {
    return;
  }
  _imageState = imageState;
  for (UIView<DSSegmentView> *view in _segmentViews) {
    if ([view isKindOfClass:DSImageSegmentView.class]) {
      [(DSImageSegmentView *)view setImageState:imageState];
    }
  }
  [self setNeedsLayout];
}

- (void)setSettled:(BOOL)settled {
  if (settled == _settled) {
    return;
  }
  _settled = settled;
  for (UIView<DSSegmentView> *view in _segmentViews) {
    if ([view isKindOfClass:DSImageSegmentView.class]) {
      ((DSImageSegmentView *)view).settled = settled;
    }
  }
}

- (void)setScreenScale:(CGFloat)screenScale {
  _screenScale = screenScale;
  for (UIView<DSSegmentView> *view in _segmentViews) {
    if ([view isKindOfClass:DSImageSegmentView.class]) {
      ((DSImageSegmentView *)view).screenScale = screenScale;
    }
  }
}

- (void)setImageDelegate:(id<DSImageDelegate>)imageDelegate {
  _imageDelegate = imageDelegate;
  for (UIView<DSSegmentView> *view in _segmentViews) {
    if ([view isKindOfClass:DSImageSegmentView.class]) {
      ((DSImageSegmentView *)view).imageDelegate = imageDelegate;
    }
  }
}

- (void)reapplyState {
  for (NSUInteger index = 0; index < _segmentViews.count; index++) {
    [self applyStateToSegmentView:_segmentViews[index] atIndex:index];
  }
  [self setNeedsLayout];
}

- (void)setControlLabels:(NSArray<NSString *> *)labels {
  _labels = [labels copy];
  for (UIView *view in _segmentViews) {
    if ([view respondsToSelector:@selector(setControlLabels:)]) {
      [(id)view setControlLabels:_labels];
    }
  }
}

- (void)setControlDelegate:(id<DSControlDelegate>)controlDelegate {
  _controlDelegate = controlDelegate;
  for (UIView *view in _segmentViews) {
    if ([view respondsToSelector:@selector(setControlDelegate:)]) {
      [(id)view setControlDelegate:controlDelegate];
    }
  }
}

- (void)resetTableScrollPositions {
  for (UIView *view in _segmentViews) {
    if ([view isKindOfClass:DSTableSegmentView.class]) {
      [(DSTableSegmentView *)view resetScrollPosition];
    }
  }
}

/// A new generation clears the reader's own scroll positions, which is what
/// `resetTableScrollPositions` has always done for tables, and what a code card's
/// vertical offset needs as well: the old offsets name lines in a document that
/// is gone.
- (void)resetScrollPositions {
  [self resetTableScrollPositions];
  for (UIView *view in _segmentViews) {
    if ([view isKindOfClass:DSCodeSegmentView.class]) {
      [(DSCodeSegmentView *)view resetScrollPosition];
    }
  }
}

- (void)setLinkDelegate:(id<DSLinkDelegate>)linkDelegate {
  _linkDelegate = linkDelegate;
  for (UIView *view in _segmentViews) {
    [(id)view setLinkDelegate:linkDelegate];
  }
}

- (void)layoutSubviews {
  [super layoutSubviews];
  if (_rendered == nil) {
    return;
  }
  // The frames are measured under the same wrap choices the code cards are
  // drawn with, so a block's frame and its drawn layout are one decision. The
  // set is the reader's own, and `codeWraps` is what the card views call too.
  // The image answers are part of the key, so a load that finished re-stacks the
  // frames rather than leaving a picture in the box it was measured at.
  NSArray<NSValue *> *frames = [_rendered segmentFramesForWidth:self.bounds.size.width
                                                          chrome:_chrome
                                                             code:_codeLayout
                                                           images:_imageState
                                                              hosts:_hostLayout
                                                             tables:_tableLayout];
  for (NSUInteger index = 0; index < _segmentViews.count && index < frames.count; index++) {
    _segmentViews[index].frame = frames[index].CGRectValue;
  }
}

- (NSInteger)linkIndexAtPoint:(CGPoint)point {
  for (UIView<DSSegmentView> *view in _segmentViews) {
    if (CGRectContainsPoint(view.frame, point)) {
      return [view linkIndexAtPoint:[self convertPoint:point toView:view]];
    }
  }
  return -1;
}

@end
