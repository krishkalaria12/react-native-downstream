#import "DSSegmentViews.h"

#import "DSLayoutManager.h"
#import "DSMathFonts.h"

namespace {

NSString *const kPlaceholder = @"\u200b";

} // namespace

#pragma mark - Text segments

@implementation DSTextSegmentView {
  DSTextKitStack *_stack;
  DSTextSegment *_segment;
  NSArray *_cachedAccessibilityElements;
  /// The document block this view's first character is. A reveal unit names a
  /// block and an offset within it, and this is what turns that into a range in
  /// this view's own text.
  NSInteger _blockOffset;
  BOOL _hosted;
  /// Whether this view's spoilers are showing. One boolean for the whole view:
  /// a tap reveals the spoiler it lands on, and a second tap hides them again.
  BOOL _spoilersRevealed;
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
    // The tap recognizer sits ahead of the text view's own, so a press on a
    // summary is a container press rather than the start of a selection. It fails
    // as soon as the touch is anywhere else, which is every other touch in a
    // document without a `<details>` in it.
    UITapGestureRecognizer *detailsTap =
        [[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(handleDetailsTap:)];
    detailsTap.delegate = (id<UIGestureRecognizerDelegate>)self;
    detailsTap.cancelsTouchesInView = YES;
    [self addGestureRecognizer:detailsTap];
  }
  return self;
}

- (void)setHosted:(BOOL)hosted {
  if (hosted == _hosted) {
    return;
  }
  _hosted = hosted;
  // A hosted block shows none of its own drawing, because the React child
  // above it is the block now: text, decorations and chevron alike.
  self.hidden = hosted;
}

- (void)setSegment:(DSTextSegment *)segment {
  if (segment == _segment) {
    return;
  }
  _spoilersRevealed = NO;
  _segment = segment;
  ((DSLayoutManager *)self.layoutManager).blockRanges = segment.blockRanges;
  [self applyDetailsState];
  DSUpdateTextStorage(_stack.textStorage, segment.attributedString);
  self.selectedRange = NSMakeRange(0, 0);
  invalidateAccessibleLinks(_cachedAccessibilityElements);
  _cachedAccessibilityElements = nil;
}

/// The document block index this view's text starts at, which is what maps a
/// reveal unit's block-and-offset pair into this view's own coordinates.
- (void)setBlockOffset:(NSInteger)blockOffset {
  _blockOffset = blockOffset;
  ((DSLayoutManager *)self.layoutManager).blockOffset = blockOffset;
}

- (void)handleDetailsTap:(UITapGestureRecognizer *)recognizer {
  if (recognizer.state != UIGestureRecognizerStateEnded) {
    return;
  }
  const CGPoint at = [recognizer locationInView:self];
  const uint32_t container = [self detailsContainerAtPoint:at];
  if (container != 0) {
    [self.controlDelegate didToggleDetailsAtContainer:container
                                                 open:![self isOpenForContainer:container]];
    return;
  }
  // No summary under the finger: a spoiler is the only other thing a press can
  // mean here, and it is hit-tested by range rather than by glyph because hidden
  // text has no ink to aim at.
  [self toggleSpoilersAtPoint:at];
}

// Summary presses and spoiler presses share this recognizer. Rejecting a
// summary here prevents handleDetailsTap: from ever receiving its press.
- (BOOL)gestureRecognizerShouldBegin:(UIGestureRecognizer *)gestureRecognizer {
  const CGPoint point = [gestureRecognizer locationInView:self];
  if ([self detailsContainerAtPoint:point] != 0) {
    return YES;
  }
  const NSUInteger at = [self.layoutManager glyphIndexForPoint:point inTextContainer:self.textContainer];
  return at < _stack.textStorage.length &&
      [_stack.textStorage attribute:DSSpoilerAttributeName atIndex:at effectiveRange:nil] != nil;
}

// A summary's single tap owns disclosure before UIKit interprets repeated
// taps as text selection. Long presses keep their native selection behavior.
- (BOOL)gestureRecognizer:(UIGestureRecognizer *)gestureRecognizer
    shouldBeRequiredToFailByGestureRecognizer:(UIGestureRecognizer *)other {
  return [other isKindOfClass:UITapGestureRecognizer.class] &&
      [self detailsContainerAtPoint:[gestureRecognizer locationInView:self]] != 0;
}

/// The layout manager is what splits the glyph runs, so the provider is its as
/// well as this view's.
/// Extra items on the text selection menu, and copy as Markdown.
///
/// A `UIMenu` built from the text view's own, so the platform's items keep
/// working and this only adds. "Copy as Markdown" copies the blocks the
/// selection touches as the Markdown that produced them — because blocks are the
/// unit the exporter reads, a selection that starts mid-paragraph copies the
/// whole block rather than a fragment no paste would render as seen.
- (void)buildMenuWithBuilder:(id<UIMenuBuilder>)builder {
  [super buildMenuWithBuilder:builder];
  if (_segment == nil) {
    return;
  }
  UIAction *copyMarkdown =
      [UIAction actionWithTitle:@"Copy as Markdown"
                         image:nil
                    identifier:@"com.downstream.copy-markdown"
                       handler:^(__kindof UIAction *_Nonnull action) {
                         [self copySelectionAsMarkdown];
                       }];
  UIMenu *menu = [UIMenu menuWithTitle:@""
                                 image:nil
                            identifier:nil
                               options:UIMenuOptionsDisplayInline
                              children:@[ copyMarkdown ]];
  // Beside the platform's standard edit menu rather than after the Copy item:
  // the action-based sibling API is gone from the SDK, and the standard menu
  // only has a public identifier from iOS 16. Below that the platform's own
  // menu stands as is, and the item is unavailable rather than misplaced.
  if (@available(iOS 16.0, *)) {
    [builder insertSiblingMenu:menu afterMenuForIdentifier:UIMenuStandardEdit];
  }
}

/// The selection as the Markdown that produced it, or nothing when there is no
/// selection to copy.
- (void)copySelectionAsMarkdown {
  if (self.selectedRange.length == 0 || _segment == nil) {
    return;
  }
  const NSRange selected = self.selectedRange;
  NSInteger first = -1;
  NSInteger last = -1;
  for (NSUInteger index = 0; index < _segment.blockRanges.count; index++) {
    const NSRange block = _segment.blockRanges[index].rangeValue;
    if (NSMaxRange(block) > selected.location && block.location < NSMaxRange(selected)) {
      if (first < 0) {
        first = (NSInteger)index;
      }
      last = (NSInteger)index;
    }
  }
  if (first < 0) {
    return;
  }
  // `blockOffset` is the document block this view's first block is, so the
  // view's own ranges map onto the document by position. The export is the same
  // one a copy produces, so both platforms can only ever produce identical
  // bytes.
  const uint32_t from = static_cast<uint32_t>(_blockOffset + first);
  const uint32_t to = static_cast<uint32_t>(_blockOffset + last + 1);
  NSString *markdown = [self.controlDelegate markdownForBlocksFrom:from to:to];
  if (markdown.length == 0) {
    return;
  }
  [UIPasteboard.generalPasteboard setString:markdown];
}

- (void)setLinkPreview:(BOOL)linkPreview {
  // UITextView offers no link preview API to forward this to: a long press is
  // the app's, through `onLinkLongPress`, on every iOS this library builds
  // for. Kept as a setter so the document view's fan-out compiles, and a
  // no-op so a prop that does nothing is visibly nothing.
  (void)linkPreview;
}

- (void)setRevealProvider:(id<DSRevealProvider>)revealProvider {
  if (_revealProvider == revealProvider) {
    return;
  }
  _revealProvider = revealProvider;
  ((DSLayoutManager *)self.layoutManager).revealProvider = revealProvider;
  [self setNeedsDisplay];
}

- (void)setDetailsToggle:(BOOL)toggle {
  if (toggle == _detailsToggle) {
    return;
  }
  _detailsToggle = toggle;
  invalidateAccessibleLinks(_cachedAccessibilityElements);
  _cachedAccessibilityElements = nil;
  [self applyDetailsState];
}

- (void)setDetailsChoices:(downstream::ReaderChoices)choices {
  if (choices == _detailsChoices) {
    return;
  }
  _detailsChoices = choices;
  invalidateAccessibleLinks(_cachedAccessibilityElements);
  _cachedAccessibilityElements = nil;
  [self applyDetailsState];
}

/// Hands the chevron's two inputs to the layout manager, which is what draws the
/// decoration beside the text rather than this view drawing over it.
- (void)applyDetailsState {
  DSLayoutManager *layout = (DSLayoutManager *)self.layoutManager;
  [layout setDetailsToggle:_detailsToggle];
  [layout setDetailsChoices:_detailsChoices];
  [self setNeedsDisplay];
}

/// A press on hidden text reveals it; a press on shown text hides it again.
///
/// The colors are this view's own storage to change: the text storage holds a
/// copy of the segment's string, so revealing here never touches the string
/// another view is drawing from. Hiding rebuilds from the segment, because the
/// segment is the only place the hidden colors still are.
- (void)toggleSpoilersAtPoint:(CGPoint)point {
  NSTextStorage *storage = _stack.textStorage;
  const NSUInteger at =
      [self.layoutManager glyphIndexForPoint:point inTextContainer:self.textContainer];
  if (at >= storage.length) {
    return;
  }
  NSRange effective;
  id spoiler = [storage attribute:DSSpoilerAttributeName atIndex:at effectiveRange:&effective];
  if (spoiler == nil) {
    return;
  }
  _spoilersRevealed = !_spoilersRevealed;
  if (_spoilersRevealed) {
    // Only the spoiler's own attributes go: the background it hid behind and
    // the foreground it cleared. Anything else on the range — a link, a style —
    // stays exactly as it was.
    [storage removeAttribute:NSBackgroundColorAttributeName range:effective];
    [storage removeAttribute:NSForegroundColorAttributeName range:effective];
  } else if (_segment != nil) {
    [_stack.textStorage setAttributedString:_segment.attributedString];
  }
  [self setNeedsDisplay];
}

/// The `<details>` container this block is the summary of, or 0.
- (uint32_t)summaryContainerIdFor:(DSBlockInfo *)info {
  if (!_detailsToggle) {
    return 0;
  }
  for (DSContainerInfo *frame in info.containers) {
    if (frame.kind == DSContainerKindDetails && frame.summary) {
      return frame.containerId;
    }
  }
  return 0;
}

/// The custom action a screen reader offers to open or close a body.
///
/// A custom action rather than the element's own activation because the
/// surrounding element is also selectable text: VoiceOver's default double-tap on
/// a static text does nothing, and an app that wants a tap-to-select gesture has
/// to keep it.
- (UIAccessibilityCustomAction *)openActionFor:(uint32_t)containerId {
  const BOOL open = [self isOpenForContainer:containerId];
  NSString *title = open ? @"Collapse" : @"Expand";
  __weak __typeof__(self) weakSelf = self;
  UIAccessibilityCustomAction *action =
      [[UIAccessibilityCustomAction alloc] initWithName:title
                                       actionHandler:^BOOL(UIAccessibilityCustomAction *customAction) {
                                         __typeof__(self) strongSelf = weakSelf;
                                         [strongSelf.controlDelegate didToggleDetailsAtContainer:containerId
                                                                                            open:!open];
                                         return YES;
                                       }];
  return action;
}

/**
 * The `<details>` container a touch lands on, or 0.
 *
 * The whole summary line is the control, because a reader aiming at a disclosure
 * aims at the words next to it and a hit test that needed the glyph to be exactly
 * right would miss more often than it hit. A link inside a summary is still a
 * summary: opening the body is what a press on that line means.
 */
- (uint32_t)detailsContainerAtPoint:(CGPoint)point {
  if (!_detailsToggle || _segment == nil) {
    return 0;
  }
  const NSUInteger at = [self.layoutManager glyphIndexForPoint:point inTextContainer:self.textContainer];
  if (at >= self.textStorage.length) {
    return 0;
  }
  NSRange blockRange;
  DSBlockInfo *info = [self.textStorage attribute:DSBlockInfoAttributeName
                                          atIndex:at
                            longestEffectiveRange:&blockRange
                                          inRange:NSMakeRange(0, self.textStorage.length)];
  if (info == nil) {
    return 0;
  }
  for (DSContainerInfo *frame in info.containers) {
    if (frame.kind == DSContainerKindDetails && frame.summary) {
      return frame.containerId;
    }
  }
  return 0;
}

/// This view's own block ranges, which the reveal provider maps units through.
/// This view's block ranges, which the document view counts blocks with.
- (NSArray<NSValue *> *)blockRanges {
  return _segment.blockRanges;
}

/// Redraws one block's characters, which is what a frame of animation asks for.
///
/// The range is in the block's own text, and this view knows where the block
/// starts in its own, so the translation is one addition. TextKit then redraws
/// only the line fragments that range touches, with no text storage edit, no glyph
/// invalidation, and no layout.
- (void)invalidateRevealCharacterRange:(NSRange)range
                            blockOffset:(NSInteger)blockOffset
                                     now:(double)now {
  (void)now;
  // `blockOffset` is the unit's document block, which can be any block this view
  // holds, not only its first.
  if (blockOffset < _blockOffset || _segment == nil || range.length == 0) {
    return;
  }
  const NSUInteger index = (NSUInteger)(blockOffset - _blockOffset);
  if (index >= _segment.blockRanges.count) {
    return;
  }
  const NSRange base = _segment.blockRanges[index].rangeValue;
  const NSRange characters = NSMakeRange(base.location + range.location, range.length);
  if (NSMaxRange(characters) > _stack.textStorage.length) {
    return;
  }
  // The background and the glyphs are separate passes over the same range, so an
  // inline-code background that outlived its glyph would leave a rectangle behind.
  [self.layoutManager invalidateDisplayForCharacterRange:characters];
}

- (void)setLinkDelegate:(id<DSLinkDelegate>)linkDelegate {
  _linkDelegate = linkDelegate;
  invalidateAccessibleLinks(_cachedAccessibilityElements);
  _cachedAccessibilityElements = nil;
}

/// Whether a container's body is showing, as the chevron drew it.
///
/// The same rule the chevron used, read back from the same frames, so a summary
/// cannot read as expanded while its glyph points the other way.
- (BOOL)isOpenForContainer:(uint32_t)containerId {
  if (_segment == nil) {
    return NO;
  }
  for (DSBlockInfo *info in _segment.blocks) {
    for (DSContainerInfo *frame in info.containers) {
      if (frame.kind == DSContainerKindDetails && frame.containerId == containerId) {
        return downstream::resolveDetails(_detailsChoices, containerId, frame.writtenOpen).open;
      }
    }
  }
  return NO;
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
  // An inline formula is one attachment character carrying its literal, and the
  // literal is what a reader copying the sentence expects.
  NSMutableString *text = [NSMutableString string];
  [_stack.textStorage enumerateAttribute:DSMathLiteralAttributeName
                                 inRange:range
                                 options:0
                              usingBlock:^(NSString *literal, NSRange part, BOOL *stop) {
                                [text appendString:literal ?: [self->_stack.textStorage.string substringWithRange:part]];
                              }];
  UIPasteboard.generalPasteboard.string = [text stringByReplacingOccurrencesOfString:kPlaceholder withString:@""];
}

- (NSString *)tagTextAtPoint:(CGPoint)point {
  const UIEdgeInsets inset = self.textContainerInset;
  const CGPoint adjusted = CGPointMake(point.x - inset.left, point.y - inset.top);
  const NSUInteger at =
      [self.layoutManager glyphIndexForPoint:adjusted inTextContainer:self.textContainer];
  if (at >= self.textStorage.length) {
    return @"";
  }
  NSRange effective;
  [self.textStorage attribute:DSTagIndexAttributeName atIndex:at effectiveRange:&effective];
  if (effective.location == NSNotFound || NSMaxRange(effective) > self.textStorage.length) {
    return @"";
  }
  return [self.textStorage.string substringWithRange:effective];
}

- (NSInteger)tagIndexAtPoint:(CGPoint)point {
  const UIEdgeInsets inset = self.textContainerInset;
  return tagIndexInStack(self.layoutManager, self.textContainer,
                         CGPointMake(point.x - inset.left, point.y - inset.top));
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
      // A summary is a button, not a line of text: a screen reader has to be able
      // to say "collapsed, button" and activate it, or the only way to reach a
      // body is to guess which words are the control.
      if (_detailsToggle && [self summaryContainerIdFor:info] != 0) {
        element.accessibilityTraits |= UIAccessibilityTraitButton;
        if (![self isOpenForContainer:[self summaryContainerIdFor:info]]) {
          element.accessibilityValue = @"collapsed";
        }
        const uint32_t container = [self summaryContainerIdFor:info];
        element.accessibilityCustomActions = @[ [self openActionFor:container] ];
      }
      [elements addObject:element];
    }
  }];
  return elements;
}

@end
