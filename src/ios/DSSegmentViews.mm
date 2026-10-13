#import "DSSegmentViews.h"
#import "DSSegmentViewport.h"

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

/// Tag index under a point in a laid-out TextKit stack, or -1.
///
/// Beside the link lookup because a press has to know which tag it landed on:
/// the whole run is the control, and the attribute names it.
NSInteger tagIndexInStack(NSLayoutManager *layoutManager, NSTextContainer *container, CGPoint location) {
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
  NSNumber *tag = [layoutManager.textStorage attribute:DSTagIndexAttributeName atIndex:character effectiveRange:NULL];
  return tag != nil ? tag.integerValue : -1;
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
NSString *const kSymbolClose = @"xmark";
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

NSString *const DSControlIconCopy = @"copy";
NSString *const DSControlIconCheck = @"check";
NSString *const DSControlIconDownload = @"download";
NSString *const DSControlIconMaximize = @"maximize";
NSString *const DSControlIconClose = @"close";
NSString *const DSControlIconWrap = @"wrap";
NSString *const DSControlIconExpand = @"expand";
NSString *const DSControlIconCollapse = @"collapse";
NSString *const DSControlIconTableFullscreen = @"tableFullscreen";
NSString *const DSControlIconDetailsChevron = @"detailsChevron";

/// The SF Symbol each default name stands for.
///
/// A table rather than a switch in the drawing code, because a symbol name is the
/// one thing about a button that is neither a colour nor a size, and it is read
/// once per button rather than once per frame.
NSString *defaultSymbolForIconName(NSString *name) {
  static NSDictionary<NSString *, NSString *> *symbols;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    symbols = @{
      DSControlIconCopy : kSymbolCopy,
      DSControlIconCheck : kSymbolCheck,
      DSControlIconDownload : kSymbolDownload,
      DSControlIconMaximize : kSymbolMaximize,
      DSControlIconClose : kSymbolClose,
      DSControlIconWrap : kSymbolWrap,
      DSControlIconExpand : kSymbolExpand,
      DSControlIconCollapse : kSymbolCollapse,
      DSControlIconTableFullscreen : kSymbolMaximize,
      // Both chevrons are the same glyph here: the drawing rotates it a quarter
      // turn when the body is open, which is the same glyph the code card's
      // expand arrow is built from.
      DSControlIconDetailsChevron : kSymbolChevronDown,
    };
  });
  NSString *const symbol = symbols[name];
  return symbol != nil ? symbol : name;
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
  /// The `icons` prop's resolved names, handed to every segment view on the way.
  NSArray<NSString *> *_icons;
  /// Whether a hold on a link may show the system's preview.
  BOOL _linkPreview;
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
  NSMutableArray<DSTextSegmentView *> *_recycledTextViews;
  NSMapTable<DSTextSegment *, NSNumber *> *_segmentEligibility;
  CGRect _segmentViewport;
  BOOL _segmentViewportKnown;
  BOOL _segmentAccessibilityExpanded;
  BOOL _segmentAccessibilityActive;
  BOOL _segmentLayoutSync;
  BOOL _hasDeferrableText;
  DSRenderedDocument *_virtualizedDocument;
  NSArray<NSValue *> *_virtualizedFrames;
  NSRange _virtualizedInterval;
  NSIndexSet *_viewportOnlyIndices;
  NSMutableIndexSet *_mountedSegmentIndices;
  BOOL _virtualizedFramesOrdered;
}

- (instancetype)initWithFrame:(CGRect)frame {
  if (self = [super initWithFrame:frame]) {
    self.backgroundColor = UIColor.clearColor;
    _segmentViews = [NSMutableArray new];
    _mountedSegmentIndices = [NSMutableIndexSet new];
    _recycledTextViews = [NSMutableArray new];
    _segmentEligibility = [NSMapTable weakToStrongObjectsMapTable];
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
/// True when the segment shows a hosted tag rather than its own text.
///
/// The segment's own container id, which the plan grouped it for: a hosted tag
/// replaces exactly that range, so the key is the container rather than a block.
- (BOOL)hostsCustomContainerAtIndex:(NSUInteger)index {
  NSArray *segments = _rendered.segments;
  if (segments == nil || index >= segments.count) {
    return NO;
  }
  id segment = segments[index];
  if (![segment isKindOfClass:DSTextSegment.class]) {
    return NO;
  }
  const uint32_t container = ((DSTextSegment *)segment).customContainer;
  return container != UINT32_MAX && _hostLayout.hostsTag(container);
}

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
  if ([view isKindOfClass:DSDeferredTextView.class]) return;
  if ([view isKindOfClass:DSTextSegmentView.class]) {
    DSTextSegmentView *text = (DSTextSegmentView *)view;
    text.selectable = _selectable && [self isSegmentSettledAtIndex:index];
    // A hosted tag replaces its whole segment: the child owns the range now,
    // so the text underneath draws nothing. Only a segment the plan grouped
    // for one container is ever hosted, which is what makes the container id
    // the key rather than a block range.
    text.hosted = [self hostsCustomContainerAtIndex:index];
    // The chevron is the control's own glyph, so it appears with the control and
    // points the way the body will move.
    text.detailsToggle = (_controls & downstream::ControlBits::DetailsToggle) != 0;
    text.detailsChoices = _detailsChoices;
    text.controlDelegate = _controlDelegate;
    text.linkPreview = _linkPreview;
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
  DSRenderedDocument *previous = _rendered;
  _rendered = rendered;
  [self updateDeferrableText];
  [self updateSegmentViewportClock];
  [self reconcileSegmentViews:previous];
  if (previous != nil && previous.documentId != rendered.documentId)
    [DSRenderedDocument retireDocumentId:previous.documentId];
}

- (void)updateDeferrableText {
  _hasDeferrableText = NO;
  if (_selectable) return;
  for (id segment in _rendered.segments) {
    if ([segment isKindOfClass:DSTextSegment.class] && [self canDeferTextSegment:segment]) {
      _hasDeferrableText = YES; break;
    }
  }
}

- (void)reconcileSegmentViews:(DSRenderedDocument *)previous {
  DSRenderedDocument *rendered = _rendered;
  NSArray *segments = rendered.segments ?: @[];
  NSArray<NSValue *> *frames = _hasDeferrableText && segments.count > 8 && !_selectable && !_segmentAccessibilityExpanded && !_segmentAccessibilityActive &&
      _segmentViewportKnown && self.bounds.size.width > 0
      ? [rendered segmentFramesForWidth:self.bounds.size.width chrome:_chrome code:_codeLayout images:_imageState
                                 hosts:_hostLayout tables:_tableLayout] : nil;
  const CGFloat overscan = std::max(CGFloat(64), _segmentViewport.size.height / 2);
  // Ordinals first: a code card's wrap state is looked up by ordinal, and the
  // view loop below is where the cards are created.
  if (_viewportOnlyIndices == nil) {
    NSMutableArray<NSNumber *> *ordinals = [NSMutableArray arrayWithCapacity:segments.count];
    for (id segment in segments) {
      [ordinals addObject:[segment isKindOfClass:DSCodeSegment.class] ? @(((DSCodeSegment *)segment).codeOrdinal) : @(-1)];
    }
    _codeOrdinals = ordinals;
  }
  for (NSUInteger index = _viewportOnlyIndices != nil ? _viewportOnlyIndices.firstIndex : 0; index < segments.count;
       index = _viewportOnlyIndices != nil ? [_viewportOnlyIndices indexGreaterThanIndex:index] : index + 1) {
    id segment = segments[index];
    UIView<DSSegmentView> *view = index < _segmentViews.count ? _segmentViews[index] : nil;
    const CGRect frame = index < frames.count ? frames[index].CGRectValue : CGRectZero;
    if (frames != nil && [segment isKindOfClass:DSTextSegment.class] && [self canDeferTextSegment:segment] &&
        (CGRectIsEmpty(_segmentViewport) || CGRectGetMinY(frame) > CGRectGetMaxY(_segmentViewport) + overscan ||
         CGRectGetMaxY(frame) < CGRectGetMinY(_segmentViewport) - overscan)) {
      if (![view isKindOfClass:DSDeferredTextView.class]) {
        [self recycleTextView:view pool:YES];
        view = [DSDeferredTextView new];
        if (index < _segmentViews.count) _segmentViews[index] = view; else [_segmentViews addObject:view];
      }
      [_mountedSegmentIndices removeIndex:index];
      continue;
    }
    [_mountedSegmentIndices addIndex:index];
    const BOOL reusable = view != nil && [view isKindOfClass:segmentViewClassFor(segment)];
    if (!reusable) {
      [self recycleTextView:view pool:NO];
      if ([segment isKindOfClass:DSTextSegment.class]) {
        DSTextSegmentView *textView = _recycledTextViews.lastObject ?: [DSTextSegmentView new];
        [_recycledTextViews removeObjectIdenticalTo:textView];
        textView.linkDelegate = _linkDelegate;
        view = textView;
      } else if ([segment isKindOfClass:DSCodeSegment.class]) {
        DSCodeSegmentView *codeView = [DSCodeSegmentView new];
        codeView.linkDelegate = _linkDelegate;
        codeView.controlDelegate = self.controlDelegate;
        [codeView setControlLabels:_labels];
        [codeView setControlIcons:_icons];
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
        [tableView setControlIcons:_icons];
        view = tableView;
      }
      if (index < _segmentViews.count) {
        _segmentViews[index] = view;
      } else {
        [_segmentViews addObject:view];
      }
      [self addSubview:view];
    }
    // Shared committed segments retain their view state and selection. Only a
    // settlement transition needs state reapplied; other setters propagate
    // changed choices, chrome and hosting independently.
    const BOOL unchanged = reusable && index < previous.segments.count && previous.segments[index] == segment &&
        index < previous.segmentBlockIndexes.count && index < rendered.segmentBlockIndexes.count &&
        [previous.segmentBlockIndexes[index] isEqual:rendered.segmentBlockIndexes[index]] &&
        (([previous.segmentBlockIndexes[index] unsignedIntegerValue] + 1 <= previous.settledBlocks) ==
         ([rendered.segmentBlockIndexes[index] unsignedIntegerValue] + 1 <= rendered.settledBlocks));
    if (unchanged) {
      continue;
    }
    if ([segment isKindOfClass:DSTextSegment.class]) {
      [(DSTextSegmentView *)view setSegment:segment];
    } else if ([segment isKindOfClass:DSCodeSegment.class]) {
      ((DSCodeSegmentView *)view).hosted = [self hostsCodeOrdinalAtIndex:index];
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
  if (_segmentViews.count > segments.count)
    [_mountedSegmentIndices removeIndexesInRange:NSMakeRange(segments.count, _segmentViews.count - segments.count)];
  while (_segmentViews.count > segments.count) {
    [self recycleTextView:_segmentViews.lastObject pool:NO];
    [_segmentViews removeLastObject];
  }
  while (_codeOrdinals.count > segments.count) {
    [_codeOrdinals removeLastObject];
  }
  _virtualizedDocument = frames != nil ? rendered : nil;
  if (frames != _virtualizedFrames) _virtualizedFramesOrdered = frames != nil && DSSegmentFramesAreOrdered(frames);
  _virtualizedFrames = frames;
  _virtualizedInterval = frames != nil && _virtualizedFramesOrdered ? DSSegmentVisibleRange(frames, _segmentViewport) : NSMakeRange(0, 0);
  if (!_segmentLayoutSync) [self setNeedsLayout];
}

- (void)recycleTextView:(UIView *)view pool:(BOOL)pool {
  [view removeFromSuperview];
  if (pool && [view isKindOfClass:DSTextSegmentView.class] && _recycledTextViews.count < 8) {
    [_recycledTextViews addObject:(DSTextSegmentView *)view];
  }
}

- (BOOL)canDeferTextSegment:(DSTextSegment *)segment {
  NSNumber *cached = [_segmentEligibility objectForKey:segment];
  if (cached != nil) return cached.boolValue;
  const BOOL eligible = DSCanDeferTextSegment(segment);
  [_segmentEligibility setObject:@(eligible) forKey:segment];
  return eligible;
}

- (void)refreshSegmentViewport {
  const BOOL known = self.window != nil && self.bounds.size.width > 0 && self.bounds.size.height > 0;
  const CGRect visible = known ? DSSegmentVisibleRect(self) : self.bounds;
  const BOOL accessible = UIAccessibilityIsVoiceOverRunning() || UIAccessibilityIsSwitchControlRunning();
  if (known == _segmentViewportKnown && CGRectEqualToRect(visible, _segmentViewport) &&
      accessible == _segmentAccessibilityActive) return;
  _segmentViewportKnown = known; _segmentViewport = visible; _segmentAccessibilityActive = accessible;
  NSArray<NSValue *> *frames = !_selectable && !accessible && !_segmentAccessibilityExpanded && known
      ? [_rendered segmentFramesForWidth:self.bounds.size.width chrome:_chrome code:_codeLayout images:_imageState
                                    hosts:_hostLayout tables:_tableLayout] : nil;
  if (_rendered == _virtualizedDocument && frames != nil && frames == _virtualizedFrames && _virtualizedFramesOrdered) {
    NSMutableIndexSet *changed = [NSMutableIndexSet indexSetWithIndexesInRange:_virtualizedInterval];
    [changed addIndexesInRange:DSSegmentVisibleRange(frames, visible)];
    _viewportOnlyIndices = changed;
  }
  @try {
    [self reconcileSegmentViews:_rendered];
    [self layoutIfNeeded];
  } @finally { _viewportOnlyIndices = nil; }
}

- (void)didMoveToWindow {
  [super didMoveToWindow];
  [self updateSegmentViewportClock];
  [self refreshSegmentViewport];
}

- (void)updateSegmentViewportClock {
  DSObserveSegmentViewport(self, self.window != nil && !_selectable &&
      !_segmentAccessibilityExpanded && _hasDeferrableText && _rendered.segments.count > 8);
}

- (NSArray *)accessibilityElements {
  // Explicit enumeration must retain the entire document's reading order even
  // before VoiceOver's status notification arrives.
  if (!_segmentAccessibilityExpanded) {
    _segmentAccessibilityExpanded = YES;
    [self updateSegmentViewportClock];
    [self reconcileSegmentViews:_rendered];
    [self layoutIfNeeded];
  }
  return [super accessibilityElements];
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
  if (_selectionColor == selectionColor || [_selectionColor isEqual:selectionColor]) {
    return;
  }
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
  if (_selectable == selectable) {
    return;
  }
  _selectable = selectable;
  [self updateDeferrableText];
  [self updateSegmentViewportClock];
  [self reconcileSegmentViews:_rendered];
  [self reapplyState];
}

- (void)setControls:(uint32_t)controls {
  if (_controls == controls) {
    return;
  }
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

/// The `icons` prop's resolved names, forwarded like the labels: only code and
/// table views answer, and a view created later reads the current value.
- (void)setControlIcons:(NSArray<NSString *> *)icons {
  _icons = [icons copy];
  for (UIView *view in _segmentViews) {
    if ([view respondsToSelector:@selector(setControlIcons:)]) {
      [(id)view setControlIcons:_icons];
    }
  }
}

/// Whether a long press shows the platform's link preview, forwarded like the
/// labels: only text views answer, and a view created later reads the current
/// value.
- (void)setLinkPreview:(BOOL)linkPreview {
  _linkPreview = linkPreview;
  for (UIView *view in _segmentViews) {
    if ([view respondsToSelector:@selector(setLinkPreview:)]) {
      [(id)view setLinkPreview:linkPreview];
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
    if ([view respondsToSelector:@selector(setLinkDelegate:)]) [(id)view setLinkDelegate:linkDelegate];
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
  // A cached frame array includes width, chrome, choices, image and host geometry.
  // Viewport changes reconcile independently; a steady UIKit layout must not
  // rebuild ordinals or visit thousands of deferred text slots again.
  if (_hasDeferrableText && !_selectable && _segmentViewportKnown &&
      !_segmentAccessibilityExpanded && !_segmentAccessibilityActive &&
      (_virtualizedDocument != _rendered || _virtualizedFrames != frames)) {
    _segmentLayoutSync = YES;
    @try { [self reconcileSegmentViews:_rendered]; }
    @finally { _segmentLayoutSync = NO; }
  }
  // Non-text and ineligible text views stay mounted even outside the viewport.
  // Include those indexes so geometry changes still update every live view.
  for (NSUInteger index = _mountedSegmentIndices.firstIndex;
       index < _segmentViews.count && index < frames.count;
       index = [_mountedSegmentIndices indexGreaterThanIndex:index]) {
    UIView *view = _segmentViews[index];
    if ([view isKindOfClass:DSDeferredTextView.class]) continue;
    const CGRect frame = frames[index].CGRectValue;
    id segment = _rendered.segments[index];
    const BOOL hostedTag = [segment isKindOfClass:DSTextSegment.class] &&
        _hostLayout.hostsTag(((DSTextSegment *)segment).customContainer);
    const BOOL hidden = frame.size.height == 0 || hostedTag;
    if (view.hidden != hidden) {
      view.hidden = hidden;
    }
    if (!CGRectEqualToRect(view.frame, frame)) {
      view.frame = frame;
    }
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

- (NSInteger)tagIndexAtPoint:(CGPoint)point {
  for (UIView<DSSegmentView> *view in _segmentViews) {
    if (!CGRectContainsPoint(view.frame, point)) {
      continue;
    }
    if ([view conformsToProtocol:@protocol(DSTagPressable)]) {
      return [(UIView<DSTagPressable> *)view tagIndexAtPoint:[self convertPoint:point toView:view]];
    }
  }
  return -1;
}

- (NSString *)tagTextAtPoint:(CGPoint)point {
  for (UIView<DSSegmentView> *view in _segmentViews) {
    if (!CGRectContainsPoint(view.frame, point)) {
      continue;
    }
    if ([view conformsToProtocol:@protocol(DSTagPressable)]) {
      return [(UIView<DSTagPressable> *)view tagTextAtPoint:[self convertPoint:point toView:view]];
    }
  }
  return @"";
}

@end
