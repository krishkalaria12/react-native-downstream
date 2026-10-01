#import "DSSegmentViews.h"

#import "DSLayoutManager.h"

#include "Controls.h"

#include <algorithm>
#include <cmath>

using namespace downstream;
// The slot names a themed drawing reads. The layout helpers they sit beside live
// in `theme_layout`, which needs no `using` here: nothing in this file asks for
// one, because the metrics arrive on the segment rather than as a rule to apply.
using namespace downstream::theme;

namespace {

constexpr CGFloat kLinkHitSlop = 4;

NSString *const kPlaceholder = @"\u200b";

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

} // namespace

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

namespace {

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
                            NSString *prefix, UIAccessibilityTraits firstTraits,
                            id<DSLinkDelegate> delegate) {
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

} // namespace

#pragma mark - Text segments

@implementation DSTextSegmentView {
  DSTextKitStack *_stack;
  DSTextSegment *_segment;
  NSArray *_cachedAccessibilityElements;
}

- (instancetype)init {
  DSLayoutManager *layoutManager = [DSLayoutManager new];
  DSTextKitStack *stack = [[DSTextKitStack alloc] initWithLayoutManager:layoutManager];
  if (self = [super initWithFrame:CGRectZero textContainer:stack.textContainer]) {
    _stack = stack;
    stack.textContainer.widthTracksTextView = YES;
    self.editable = NO;
    self.selectable = NO;
    self.scrollEnabled = NO;
    self.backgroundColor = UIColor.clearColor;
    self.textContainerInset = UIEdgeInsetsZero;
    self.dataDetectorTypes = UIDataDetectorTypeNone;
    self.isAccessibilityElement = NO;
    self.textDragInteraction.enabled = NO;
  }
  return self;
}

- (void)setSegment:(DSTextSegment *)segment {
  if (segment == _segment) {
    return;
  }
  _segment = segment;
  [_stack.textStorage setAttributedString:segment.attributedString];
  self.selectedRange = NSMakeRange(0, 0);
  invalidateAccessibleLinks(_cachedAccessibilityElements);
  _cachedAccessibilityElements = nil;
}

- (void)setLinkDelegate:(id<DSLinkDelegate>)linkDelegate {
  _linkDelegate = linkDelegate;
  invalidateAccessibleLinks(_cachedAccessibilityElements);
  _cachedAccessibilityElements = nil;
}

/// UITextView draws its selection in the view's tint color, which is the only
/// handle on it that a deployment target still declares: `selectionColor` was
/// withdrawn from the headers in favour of the iOS 18 highlight attributes,
/// which color a text selection's own background rather than the selection
/// itself. Nothing else here is tinted, because this view draws its text, its
/// links and its own chrome with explicit colors.
- (void)setSelectionColor:(UIColor *)selectionColor {
  _selectionColor = selectionColor;
  self.tintColor = selectionColor;
}

- (void)layoutSubviews {
  [super layoutSubviews];
  invalidateAccessibleLinks(_cachedAccessibilityElements);
  _cachedAccessibilityElements = nil;
}

- (void)traitCollectionDidChange:(UITraitCollection *)previousTraitCollection {
  [super traitCollectionDidChange:previousTraitCollection];
  if ([self.traitCollection hasDifferentColorAppearanceComparedToTraitCollection:previousTraitCollection]) {
    [self setNeedsDisplay];
  }
}

// Copy plain visible text; internal attributes and placeholders stay inside.
- (void)copy:(id)sender {
  const NSRange range = self.selectedRange;
  if (range.length == 0) {
    return;
  }
  NSString *text = [[_stack.textStorage.string substringWithRange:range] stringByReplacingOccurrencesOfString:kPlaceholder
                                                                                                   withString:@""];
  UIPasteboard.generalPasteboard.string = text;
}

- (NSInteger)linkIndexAtPoint:(CGPoint)point {
  const UIEdgeInsets inset = self.textContainerInset;
  return linkIndexInStack(self.layoutManager, self.textContainer,
                          CGPointMake(point.x - inset.left, point.y - inset.top));
}

- (NSArray *)accessibilityElements {
  if (_cachedAccessibilityElements == nil) {
    _cachedAccessibilityElements = [self buildAccessibilityElements];
  }
  return _cachedAccessibilityElements;
}

- (NSArray *)buildAccessibilityElements {
  if (_segment == nil) {
    return @[];
  }
  DSLayoutManager *layoutManager = (DSLayoutManager *)self.layoutManager;
  [layoutManager ensureLayoutForTextContainer:self.textContainer];
  NSTextStorage *storage = _stack.textStorage;
  const CGFloat top = self.textContainerInset.top;
  NSMutableArray *elements = [NSMutableArray arrayWithCapacity:_segment.blocks.count];

  [_segment.blocks enumerateObjectsUsingBlock:^(DSBlockInfo *info, NSUInteger index, BOOL *stop) {
    const NSRange range = self->_segment.blockRanges[index].rangeValue;
    DSBlockTextBounds bounds;
    if (info.decoration == DSBlockDecorationRule || ![layoutManager textBoundsForCharacterRange:range bounds:&bounds]) {
      return;
    }
    const CGRect blockFrame = CGRectMake(0, top + bounds.top, self.bounds.size.width, bounds.bottom - bounds.top);
    if (hasLink(storage, range)) {
      appendAccessibleSlices(elements, storage, range, self, layoutManager, self.textContainer,
                             CGPointMake(self.textContainerInset.left, top), blockFrame,
                             info.accessibilityPrefix,
                             info.headingLevel > 0 ? UIAccessibilityTraitHeader : 0, self.linkDelegate);
    } else {
      NSString *text = [storage.string substringWithRange:range];
      UIAccessibilityElement *element = [[UIAccessibilityElement alloc] initWithAccessibilityContainer:self];
      element.accessibilityLabel = info.accessibilityPrefix != nil
          ? [NSString stringWithFormat:@"%@, %@", info.accessibilityPrefix, text] : text;
      element.accessibilityFrameInContainerSpace = blockFrame;
      element.accessibilityTraits = info.headingLevel > 0 ? UIAccessibilityTraitHeader : UIAccessibilityTraitStaticText;
      [elements addObject:element];
    }
  }];
  return elements;
}

@end

#pragma mark - Control buttons

/// 28 points of glyph in a 44-point target, which is the smallest comfortable
/// touch target on the platform.
constexpr CGFloat kButtonGlyph = 28;
/// The height of a control row, matching the one the measurement reserves.
constexpr CGFloat kChromeRowHeight = 32;
/// How long a copy button shows its check before going back, as in Streamdown.
constexpr NSTimeInterval kCopiedDuration = 2.0;

using namespace downstream::ControlBits;

/// One action on a code block or a table. The view that owns the button sends
/// the delegate a description rather than doing the work itself, because only
/// the view that holds the document can read a block.
@interface DSControlButton : UIButton
@property (nonatomic) NSString *action;
@property (nonatomic) NSString *format;
@end

@implementation DSControlButton
@end

/// Indexes into the label array `src/controls.ts` sends, in the same order.
typedef NS_ENUM(NSInteger, DSControlLabel) {
  DSControlLabelCopyCode = 0,
  DSControlLabelDownloadFile = 1,
  DSControlLabelCopied = 2,
  DSControlLabelWrapCode = 3,
  DSControlLabelUnwrapCode = 4,
  DSControlLabelCopyTable = 5,
  DSControlLabelCopyTableAsMarkdown = 6,
  DSControlLabelCopyTableAsCsv = 7,
  DSControlLabelCopyTableAsTsv = 8,
  DSControlLabelDownloadTable = 9,
  DSControlLabelDownloadTableAsCsv = 10,
  DSControlLabelDownloadTableAsMarkdown = 11,
  DSControlLabelTableFormatMarkdown = 12,
  DSControlLabelTableFormatCsv = 13,
  DSControlLabelTableFormatTsv = 14,
  DSControlLabelCount = 15,
};

NSString *const kSymbolCopy = @"doc.on.doc";
NSString *const kSymbolCheck = @"checkmark";
NSString *const kSymbolDownload = @"arrow.down.to.line";

/// Wrapping is a state, so it is drawn as one: a glyph with an arrow through
/// it when the block wraps and a plain one when it does not. SF Symbols has no
/// wrap glyph, so the state is read from the button's selected trait and the
/// arrow is drawn rather than looked up.
NSString *const kSymbolWrap = @"text.alignleft";
NSString *const kSymbolUnwrap = @"text.alignleft";

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

namespace {
} // namespace

#pragma mark - Code cards

/// A code card: a fixed header row, and below it a horizontal scroll view
/// holding a text segment view for the one code block.
///
/// The header does not scroll, so the buttons stay in place while a long line
/// scrolls, and it takes layout space rather than floating over the code, so
/// the code is never covered.
@implementation DSCodeSegmentView {
  DSCodeSegment *_segment;
  UIView *_header;
  UILabel *_language;
  UIStackView *_buttons;
  UIScrollView *_scrollView;
  DSTextSegmentView *_body;
  NSArray<NSString *> *_labels;
  /// The header row's text color, from the theme, which the buttons share so a
  /// themed header and its controls read as one row.
  UIColor *_headerColor;
  /// The labels the current buttons and menu items were built from, so a changed
  /// `translations` prop rebuilds them instead of leaving stale strings up.
  NSArray<NSString *> *_builtLabels;
  BOOL _wrapped;
  BOOL _copied;
  NSTimer *_copiedTimer;
  CGFloat _savedOffset;
  BOOL _needsScrollRestore;
}

- (instancetype)initWithFrame:(CGRect)frame {
  if (self = [super initWithFrame:frame]) {
    self.opaque = NO;
    self.backgroundColor = UIColor.clearColor;
    self.contentMode = UIViewContentModeRedraw;

    _header = [UIView new];
    _header.userInteractionEnabled = YES;
    _header.translatesAutoresizingMaskIntoConstraints = NO;
    _language = [UILabel new];
    _language.font = [UIFont monospacedSystemFontOfSize:12 weight:UIFontWeightRegular];
    _language.textColor = UIColor.secondaryLabelColor;
    _language.adjustsFontSizeToFitWidth = YES;
    _language.minimumScaleFactor = 0.7;
    _language.translatesAutoresizingMaskIntoConstraints = NO;

    _buttons = [UIStackView new];
    _buttons.axis = UILayoutConstraintAxisHorizontal;
    _buttons.spacing = 2;
    _buttons.translatesAutoresizingMaskIntoConstraints = NO;
    [_header addSubview:_language];
    [_header addSubview:_buttons];

    _scrollView = [UIScrollView new];
    // A vertical drag belongs to the chat list, so the scroll view takes only
    // horizontal gestures and never bounces vertically.
    _scrollView.showsVerticalScrollIndicator = NO;
    _scrollView.alwaysBounceVertical = NO;
    _scrollView.directionalLockEnabled = YES;

    // The scroll view and the body are framed in layoutSubviews from the
    // measured body size. Under Auto Layout a non-scrolling text view would
    // take its intrinsic one-line width instead, and never wrap.
    _body = [DSTextSegmentView new];
    _body.scrollEnabled = NO;

    [_scrollView addSubview:_body];
    [self addSubview:_header];
    [self addSubview:_scrollView];
    [self setConstraints];
  }
  return self;
}

- (void)setConstraints {
  [NSLayoutConstraint activateConstraints:@[
    [_header.topAnchor constraintEqualToAnchor:self.topAnchor],
    [_header.leadingAnchor constraintEqualToAnchor:self.leadingAnchor],
    [_header.trailingAnchor constraintEqualToAnchor:self.trailingAnchor],
    [_header.heightAnchor constraintEqualToConstant:_showsHeader ? kChromeRowHeight : 0],
    [_language.leadingAnchor constraintEqualToAnchor:_header.leadingAnchor constant:12],
    [_language.centerYAnchor constraintEqualToAnchor:_header.centerYAnchor],
    [_language.trailingAnchor constraintLessThanOrEqualToAnchor:_buttons.leadingAnchor constant:-8],
    [_buttons.trailingAnchor constraintEqualToAnchor:_header.trailingAnchor constant:-4],
    [_buttons.centerYAnchor constraintEqualToAnchor:_header.centerYAnchor],
  ]];
}

- (NSArray<NSString *> *)labels {
  static NSArray<NSString *> *fallback;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    fallback = @[
      @"Copy Code", @"Download file", @"Copied", @"Wrap lines", @"Don't wrap lines", @"Copy table",
      @"Copy table as Markdown", @"Copy table as CSV", @"Copy table as TSV", @"Download table",
      @"Download table as CSV", @"Download table as Markdown", @"Markdown", @"CSV", @"TSV",
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

- (void)setSegment:(DSCodeSegment *)segment {
  if (segment == _segment) {
    return;
  }
  // The tail block is rebuilt on every snapshot while its fence is open, so
  // the offset is kept only while the block still looks like the same one.
  const BOOL sameBlock = _segment != nil && segment != nil &&
                         _segment.codeOrdinal == segment.codeOrdinal &&
                         [_segment.firstLine isEqualToString:segment.firstLine];
  _savedOffset = sameBlock ? _scrollView.contentOffset.x : 0;
  _needsScrollRestore = YES;
  _segment = segment;
  _body.linkDelegate = self.linkDelegate;
  [_body setSegment:segment.text];
  _language.text = segment.language;
  _language.accessibilityLabel = segment.language.length > 0
      ? [NSString stringWithFormat:@"%@, code", segment.language]
      : @"code";
  // The header row's own three colors, read once per block rather than per draw,
  // because the card is redrawn whenever its bounds change.
  _headerColor = [segment.theme colorForSlot:kSlotCodeBlockHeaderColor fallback:UIColor.secondaryLabelColor];
  _language.textColor = _headerColor;
  _language.font = segment.headerFont;
  _header.backgroundColor = [segment.theme colorForSlot:kSlotCodeBlockHeaderBackgroundColor fallback:nil];
  // The buttons take their own color when the theme gave them one, so a header
  // row can have quiet controls and loud text, or the reverse.
  UIColor *buttonColor = segment.headerButtonColor ?: _headerColor;
  for (UIView *button in _buttons.arrangedSubviews) {
    [(UIControl *)button setTintColor:buttonColor];
  }
  [self updateButtons];
  [self setNeedsLayout];
  [self setNeedsDisplay];
}

- (void)setLinkDelegate:(id<DSLinkDelegate>)linkDelegate {
  _linkDelegate = linkDelegate;
  _body.linkDelegate = linkDelegate;
}

- (void)setSelectionColor:(UIColor *)selectionColor {
  _selectionColor = selectionColor;
  _body.selectionColor = selectionColor;
}

- (void)setShowsHeader:(BOOL)showsHeader {
  if (showsHeader == _showsHeader) {
    return;
  }
  _showsHeader = showsHeader;
  _header.hidden = !self.showsHeader;
  for (NSLayoutConstraint *constraint in _header.constraints) {
    if (constraint.firstAttribute == NSLayoutAttributeHeight) {
      constraint.constant = self.showsHeader ? kChromeRowHeight : 0;
    }
  }
}

- (void)setControls:(uint32_t)controls {
  if (controls == _controls) {
    return;
  }
  _controls = controls;
  [self updateButtons];
}

- (void)setControlsEnabled:(BOOL)enabled {
  if (enabled == _controlsEnabled) {
    return;
  }
  _controlsEnabled = enabled;
  [self updateButtons];
}

- (void)setWrapped:(BOOL)wrapped {
  if (wrapped == _wrapped) {
    return;
  }
  _wrapped = wrapped;
  [self updateButtons];
  [self setNeedsLayout];
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
  UIButton *wrap = makeControlButton(symbolForWrap(NO), [self label:DSControlLabelWrapCode], @"wrap", @"");
  UIButton *download =
      makeControlButton(kSymbolDownload, [self label:DSControlLabelDownloadFile], @"download", @"text");
  UIButton *copy = makeControlButton(kSymbolCopy, [self label:DSControlLabelCopyCode], @"copy", @"text");
  wrap.tag = 0;
  download.tag = 1;
  copy.tag = 2;
  for (UIButton *button in @[wrap, download, copy]) {
    button.accessibilityIdentifier = [NSString stringWithFormat:@"ds.code.%@", button.accessibilityLabel];
    [button addTarget:self action:@selector(handleControl:) forControlEvents:UIControlEventTouchUpInside];
    [_buttons addArrangedSubview:button];
  }
}

/// Rebuilds the header's buttons from the current controls, enabled state, and
/// wrap state. The buttons are created once; only their glyphs, labels, and
/// states change, which is what keeps a snapshot from allocating any.
- (void)updateButtons {
  if (!self.showsHeader) {
    _buttons.hidden = YES;
    _language.hidden = YES;
    return;
  }
  _buttons.hidden = NO;
  _language.hidden = NO;
  [self buildButtonsIfNeeded];
  NSArray<UIButton *> *all = _buttons.arrangedSubviews;
  const BOOL showDownload = (_controls & CodeDownload) != 0;
  const BOOL showCopy = (_controls & CodeCopy) != 0;
  const BOOL showWrap = (_controls & CodeWrap) != 0;
  ((UIButton *)all[0]).hidden = !showWrap;
  ((UIButton *)all[1]).hidden = !showDownload;
  ((UIButton *)all[2]).hidden = !showCopy;
  for (UIButton *button in all) {
    button.enabled = _controlsEnabled && !button.hidden;
  }

  UIButton *wrap = (UIButton *)all[0];
  wrap.selected = _wrapped;
  [wrap setImage:controlImage(symbolForWrap(_wrapped)) forState:UIControlStateNormal];
  wrap.accessibilityLabel = _wrapped ? [self label:DSControlLabelUnwrapCode] : [self label:DSControlLabelWrapCode];
  // No explicit traits: UIButton derives Selected and Not Enabled itself, and a
  // fixed value would tell VoiceOver a disabled button can be pressed.

  UIButton *download = (UIButton *)all[1];
  download.accessibilityLabel = [self label:DSControlLabelDownloadFile];
  UIButton *copy = (UIButton *)all[2];
  [copy setImage:controlImage(symbolForCopy(_copied)) forState:UIControlStateNormal];
  copy.accessibilityLabel = _copied ? [self label:DSControlLabelCopied] : [self label:DSControlLabelCopyCode];
  [self updateCopiedSpokenText];
}

- (void)updateCopiedSpokenText {
  // The live region is a sibling of the buttons, not one of them, so it is
  // found here; looking in the stack would never match and every copy would
  // leave another hidden label behind.
  UILabel *spoken = (UILabel *)[self viewWithTag:99];
  if (spoken != nil) {
    [spoken removeFromSuperview];
  }
  if (!_copied) {
    return;
  }
  // A live region is how VoiceOver hears a copy that changed no text, which a
  // button's own label change alone does not announce.
  spoken = [UILabel new];
  spoken.tag = 99;
  spoken.text = [self label:DSControlLabelCopied];
  spoken.accessibilityTraits = UIAccessibilityTraitStaticText;
  spoken.hidden = YES;
  [self addSubview:spoken];
}

- (void)handleControl:(UIButton *)sender {
  DSControlButton *button = (DSControlButton *)sender;
  if (!sender.enabled || _segment == nil) {
    return;
  }
  if ([button.action isEqualToString:@"wrap"]) {
    [self.controlDelegate didToggleWrapAtOrdinal:_segment.codeOrdinal
                                        language:_segment.language
                                         wrapped:!_wrapped];
    return;
  }
  if ([button.action isEqualToString:@"copy"]) {
    [self markCopied];
  }
  [self.controlDelegate didPressControlOfKind:DSBlockKindCode
                                   blockIndex:_segment.blockIndex
                                   blockCount:1
                                     language:_segment.language
                                      action:button.action
                                      format:button.format
                                    filename:@""];
}

- (void)markCopied {
  _copied = YES;
  [_copiedTimer invalidate];
  _copiedTimer = [NSTimer scheduledTimerWithTimeInterval:kCopiedDuration
                                                 repeats:NO
                                                   block:^(NSTimer *timer) {
                                                     self->_copied = NO;
                                                     [self updateButtons];
                                                   }];
  UIAccessibilityPostNotification(UIAccessibilityAnnouncementNotification, [self label:DSControlLabelCopied]);
  [self updateButtons];
}

- (void)dealloc {
  [_copiedTimer invalidate];
}

- (void)layoutSubviews {
  [super layoutSubviews];
  if (_segment == nil) {
    return;
  }
  const CGFloat headerHeight = self.showsHeader ? kChromeRowHeight : 0;
  const CGFloat leading = _segment.contentLeading;
  const CGFloat padding = _segment.padding;
  const CGRect card = CGRectMake(leading, headerHeight, self.bounds.size.width - leading, self.bounds.size.height - headerHeight);
  const CGFloat bodyWidth = [_segment bodyWidthForWidth:card.size.width wrapped:_wrapped];
  const CGFloat bodyHeight = [_segment bodyHeightForWidth:card.size.width wrapped:_wrapped];
  _scrollView.frame = CGRectInset(card, padding, padding);
  _body.frame = CGRectMake(0, 0, std::max<CGFloat>(bodyWidth, 0), std::max<CGFloat>(bodyHeight, 0));
  _scrollView.contentSize = _body.bounds.size;
  if (_needsScrollRestore) {
    const CGFloat maximum = std::max<CGFloat>(0, _scrollView.contentSize.width - _scrollView.bounds.size.width);
    _scrollView.contentOffset = CGPointMake(std::clamp(_savedOffset, 0.0, maximum), 0);
    _needsScrollRestore = NO;
  }
}

/// The card's rounded code background, plus the container bars and the list
/// marker, which live in the non-scrolling margin beside it.
- (void)drawRect:(CGRect)rect {
  if (_segment == nil) {
    return;
  }
  const CGFloat leading = _segment.contentLeading;
  const CGFloat headerHeight = self.showsHeader ? kChromeRowHeight : 0;
  // A rule under the header row, when the theme asked for one. Its color
  // defaults to clear, so a theme that says nothing draws nothing.
  UIColor *headerBorder = _segment.headerBorderColor;
  if (headerBorder != nil && self.showsHeader) {
    [headerBorder setFill];
    UIRectFill(CGRectMake(0, headerHeight - 1, self.bounds.size.width, 1));
  }
  const CGRect card = CGRectMake(leading, headerHeight, self.bounds.size.width - leading,
                                 self.bounds.size.height - headerHeight);
  // The card's fill, radius and border are all the theme's, and the border is
  // drawn rather than filled because it sits inside the card's bounds, which is
  // what keeps a themed card the height the measurement gave it.
  UIColor *fill = [_segment.theme colorForSlot:kSlotCodeBlockBackgroundColor fallback:UIColor.tertiarySystemFillColor];
  UIBezierPath *path = [UIBezierPath bezierPathWithRoundedRect:card cornerRadius:_segment.borderRadius];
  [fill setFill];
  [path fill];
  if (_segment.borderWidth > 0) {
    [[_segment.theme colorForSlot:kSlotCodeBlockBorderColor fallback:nil] setStroke];
    path.lineWidth = _segment.borderWidth;
    [path stroke];
  }

  for (DSContainerInfo *container in _segment.containers) {
    if (container.barColor != nil && container.barWidth > 0) {
      [container.barColor setFill];
      const CGFloat x = container.leading + 2;
      UIRectFill(CGRectMake(x, headerHeight, container.barWidth, self.bounds.size.height - headerHeight));
    }
    if (container.marker != nil) {
      UIFont *font = [container.marker attribute:NSFontAttributeName atIndex:0 effectiveRange:NULL];
      const CGFloat baseline = headerHeight + _segment.padding + font.ascender;
      const CGFloat x = container.leading + container.width - container.markerGap - container.marker.size.width;
      [container.marker drawAtPoint:CGPointMake(x, baseline - font.ascender)];
    }
  }
}

- (void)traitCollectionDidChange:(UITraitCollection *)previousTraitCollection {
  [super traitCollectionDidChange:previousTraitCollection];
  if ([self.traitCollection hasDifferentColorAppearanceComparedToTraitCollection:previousTraitCollection]) {
    [self setNeedsDisplay];
  }
}

- (BOOL)pointInside:(CGPoint)point withEvent:(UIEvent *)event {
  // The glyphs are 28 points; the target is 44, which is what a fingertip
  // needs, so the header's own bounds are widened rather than the buttons'.
  return CGRectContainsPoint(CGRectInset(self.bounds, -6, -6), point);
}

- (NSInteger)linkIndexAtPoint:(CGPoint)point {
  return [_body linkIndexAtPoint:[self convertPoint:point toView:_body]];
}

- (NSArray *)accessibilityElements {
  // Header label, then the buttons, then the code, which is the order
  // Streamdown's DOM reads in.
  if (_segment == nil) {
    return @[];
  }
  NSMutableArray *elements = [NSMutableArray new];
  UIAccessibilityElement *label = [[UIAccessibilityElement alloc] initWithAccessibilityContainer:self];
  label.accessibilityLabel = _language.accessibilityLabel;
  label.accessibilityFrameInContainerSpace = _language.frame;
  [elements addObject:label];
  for (UIView *view in _buttons.arrangedSubviews) {
    if (!view.hidden && [view isAccessibilityElement]) {
      [elements addObject:view];
    }
  }
  // The body is listed as a container, not flattened: its elements' frames are
  // in the body's space, which the header and the scroll offset move.
  [elements addObject:_body];
  return elements;
}

@end

#pragma mark - Tables

@interface DSTableGridView : UIView
@property (nonatomic, nullable) DSTableSegment *segment;
@property (nonatomic, weak, nullable) id<DSLinkDelegate> linkDelegate;
- (NSInteger)linkIndexAtPoint:(CGPoint)point;
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

- (void)drawRect:(CGRect)rect {
  if (_segment == nil) {
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
  NSUInteger firstDataRow = _segment.hasHeader ? 1 : 0;
  [_segment.rows enumerateObjectsUsingBlock:^(NSArray<DSTableCell *> *row, NSUInteger rowIndex, BOOL *stop) {
    if (self->_rowOffsets[rowIndex] > CGRectGetMaxY(rect)) {
      *stop = YES;
      return;
    }
    if (self->_rowOffsets[rowIndex + 1] < CGRectGetMinY(rect)) {
      return;
    }
    if (rowIndex >= firstDataRow) {
      UIColor *stripe = [self->_segment.theme colorForSlot:(rowIndex - firstDataRow) % 2 == 0
                                                          ? kSlotTableRowEvenBackgroundColor
                                                          : kSlotTableRowOddBackgroundColor
                                                   fallback:nil];
      if (stripe != nil) {
        [stripe setFill];
        UIRectFill(CGRectMake(0, self->_rowOffsets[rowIndex], size.width,
                              self->_rowOffsets[rowIndex + 1] - self->_rowOffsets[rowIndex]));
      }
    }
    [row enumerateObjectsUsingBlock:^(DSTableCell *cell, NSUInteger column, BOOL *innerStop) {
      const CGRect content = [self contentRectForRow:rowIndex column:column];
      if (CGRectIntersectsRect(content, rect)) {
        [cell.text drawWithRect:content options:options context:nil];
      }
    }];
  }];

  [[_segment.theme colorForSlot:kSlotTableBorderColor fallback:UIColor.separatorColor] setFill];
  CGFloat y = 0;
  UIRectFill(CGRectMake(0, 0, size.width - hairline, hairline));
  for (NSNumber *height in _segment.rowHeights) {
    y += height.doubleValue;
    UIRectFill(CGRectMake(0, y - hairline, size.width - hairline, hairline));
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

@implementation DSTableSegmentView {
  DSTableSegment *_segment;
  UIView *_toolbar;
  UIStackView *_buttons;
  UIScrollView *_scrollView;
  DSTableGridView *_grid;
  NSArray<NSString *> *_labels;
  /// The labels the current buttons and menu items were built from, so a changed
  /// `translations` prop rebuilds them instead of leaving stale strings up.
  NSArray<NSString *> *_builtLabels;
  BOOL _needsScrollRestore;
  BOOL _preserveScrollPosition;
  BOOL _forceScrollReset;
  CGFloat _savedScrollPosition;
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
    _scrollView = [UIScrollView new];
    _scrollView.showsVerticalScrollIndicator = NO;
    _scrollView.alwaysBounceVertical = NO;
    _scrollView.directionalLockEnabled = YES;
    _grid = [DSTableGridView new];
    [_scrollView addSubview:_grid];
    [self addSubview:_toolbar];
    [self addSubview:_scrollView];
  }
  return self;
}

- (NSArray<NSString *> *)labels {
  static NSArray<NSString *> *fallback;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    fallback = @[
      @"Copy Code", @"Download file", @"Copied", @"Wrap lines", @"Don't wrap lines", @"Copy table",
      @"Copy table as Markdown", @"Copy table as CSV", @"Copy table as TSV", @"Download table",
      @"Download table as CSV", @"Download table as Markdown", @"Markdown", @"CSV", @"TSV",
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
  copy.tag = 0;
  download.tag = 1;
  [_buttons addArrangedSubview:copy];
  [_buttons addArrangedSubview:download];
  for (UIButton *button in @[copy, download]) {
    button.accessibilityIdentifier = [NSString stringWithFormat:@"ds.table.%@", button.accessibilityLabel];
  }
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
  for (UIButton *button in all) {
    button.enabled = _controlsEnabled && !button.hidden;
  }
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
  _scrollView.frame = CGRectMake(_segment.rightToLeft ? 0 : leading, toolbarHeight,
                                 std::max<CGFloat>(0, self.bounds.size.width - leading),
                                 self.bounds.size.height - toolbarHeight);
  _grid.frame = CGRectMake(0, 0, _segment.tableSize.width, _segment.tableSize.height);
  _scrollView.contentSize = _segment.tableSize;
  if (_needsScrollRestore) {
    const CGFloat maximum = std::max<CGFloat>(0, _segment.tableSize.width - _scrollView.bounds.size.width);
    const CGFloat position = _preserveScrollPosition ? _savedScrollPosition : 0;
    _scrollView.contentOffset = CGPointMake(_segment.rightToLeft
        ? std::clamp(maximum - position, 0.0, maximum)
        : std::clamp(position, 0.0, maximum), 0);
    _needsScrollRestore = NO;
    _forceScrollReset = NO;
  }
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
    if (container.marker != nil) {
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

#pragma mark - Document

@implementation DSDocumentView {
  DSRenderedDocument *_rendered;
  NSMutableArray<UIView<DSSegmentView> *> *_segmentViews;
  NSArray<NSString *> *_labels;
  uint32_t _chrome;
  /// A code block's ordinals, so a wrap choice can be applied by ordinal
  /// rather than by segment index, which moves as blocks stream in.
  NSMutableArray<NSNumber *> *_codeOrdinals;
  /// `_codeWrapToggles` sorted, which is the form `downstream::codeWraps` takes.
  /// Both the card views and the frame stack read this one copy, so a block's
  /// drawn layout and its measured height are the same decision.
  std::vector<uint32_t> _wrapToggleOrdinals;
  /// The theme's selection color, applied to every text view in the stack.
  UIColor *_selectionColor;
}

- (instancetype)initWithFrame:(CGRect)frame {
  if (self = [super initWithFrame:frame]) {
    self.backgroundColor = UIColor.clearColor;
    _segmentViews = [NSMutableArray new];
  }
  return self;
}

static Class segmentViewClass(BOOL isText, BOOL isCode) {
  if (isText) {
    return DSTextSegmentView.class;
  }
  return isCode ? DSCodeSegmentView.class : DSTableSegmentView.class;
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

- (void)applyStateToSegmentView:(UIView<DSSegmentView> *)view atIndex:(NSUInteger)index {
  if ([view isKindOfClass:DSTextSegmentView.class]) {
    ((DSTextSegmentView *)view).selectable = _selectable && [self isSegmentSettledAtIndex:index];
    return;
  }
  if ([view isKindOfClass:DSCodeSegmentView.class]) {
    DSCodeSegmentView *code = (DSCodeSegmentView *)view;
    code.showsHeader = (_chrome & downstream::kChromeCodeHeader) != 0;
    code.controls = _controls;
    code.controlsEnabled = [self isSegmentSettledAtIndex:index];
    if (_codeOrdinals.count > index) {
      // The same helper the measurement used, so the frame this card is given
      // and the layout it draws inside it are one decision.
      code.wrapped = downstream::codeWraps(_codeBlockWrap, _wrapToggleOrdinals,
                                           _codeOrdinals[index].unsignedIntValue);
    }
    return;
  }
  DSTableSegmentView *table = (DSTableSegmentView *)view;
  table.showsToolbar = (_chrome & downstream::kChromeTableToolbar) != 0;
  table.controls = _controls;
  table.controlsEnabled = [self isSegmentSettledAtIndex:index];
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
    const Class text = DSTextSegmentView.class;
    const Class code = DSCodeSegmentView.class;
    const Class table = DSTableSegmentView.class;
    UIView<DSSegmentView> *view = index < _segmentViews.count ? _segmentViews[index] : nil;
    const BOOL reusable = view != nil && [view isKindOfClass:segmentViewClass(
                                                        [segment isKindOfClass:DSTextSegment.class],
                                                        [segment isKindOfClass:DSCodeSegment.class])];
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
    } else {
      [(DSTableSegmentView *)view setSegment:segment];
    }
    (void)text;
    (void)code;
    (void)table;
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

- (void)setChrome:(uint32_t)chrome {
  if (chrome == _chrome) {
    return;
  }
  _chrome = chrome;
  [self reapplyState];
}

- (void)setCodeBlockWrap:(BOOL)codeBlockWrap {
  if (codeBlockWrap == _codeBlockWrap) {
    return;
  }
  _codeBlockWrap = codeBlockWrap;
  [self reapplyState];
}

- (void)setCodeWrapToggles:(NSSet<NSNumber *> *)codeWrapToggles {
  if ([_codeWrapToggles isEqualToSet:codeWrapToggles]) {
    return;
  }
  _codeWrapToggles = [codeWrapToggles copy];
  _wrapToggleOrdinals.clear();
  _wrapToggleOrdinals.reserve(codeWrapToggles.count);
  for (NSNumber *ordinal in codeWrapToggles) {
    _wrapToggleOrdinals.push_back(ordinal.unsignedIntValue);
  }
  std::sort(_wrapToggleOrdinals.begin(), _wrapToggleOrdinals.end());
  [self reapplyState];
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
  NSArray<NSValue *> *frames = [_rendered segmentFramesForWidth:self.bounds.size.width
                                                          chrome:_chrome
                                                   codeBlockWrap:_codeBlockWrap
                                                     wrapToggles:_wrapToggleOrdinals];
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
