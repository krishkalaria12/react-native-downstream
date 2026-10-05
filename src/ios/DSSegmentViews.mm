#import "DSSegmentViews.h"

#import "DSFileExporter.h"
#import "DSImageLoader.h"
#import "DSLayoutManager.h"
#import "DSMathFonts.h"
#import "DSRenderedDocument.h"

#include "Controls.h"
#include "ImageSizeCache.h"
#include "math/MathSpeech.h"

#include <algorithm>
#include <cmath>
#include <string>

using namespace downstream;
// The slot names a themed drawing reads. The layout helpers they sit beside live
// in `theme_layout`, which needs no `using` here: nothing in this file asks for
// one, because the metrics arrive on the segment rather than as a rule to apply.
using namespace downstream::theme;

namespace {

constexpr CGFloat kLinkHitSlop = 4;

/// How far a line number is dimmed below the code it numbers, which is what
/// Streamdown's `muted-foreground/50` gutter draws at. Applied to the theme's own
/// code color rather than to a fixed gray, so a theme that restyles code restyles
/// its numbers in step.
constexpr CGFloat kGutterAlpha = 0.5;

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
@property (nonatomic, copy) NSString *action;
@property (nonatomic, copy) NSString *format;
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

#pragma mark - Code cards

namespace {

/// Padding inside a code card, the same on every side, and the height of the fade
/// that says a capped block has more below it. Both are the measurement's, so the
/// card draws inside the box the height reserved.
constexpr CGFloat kCodePadding = 10;
constexpr CGFloat kBottomFade = 24;

/// How far from the bottom a reader may scroll and still be "following" the
/// newest line. Streamdown's `usePinnedScroll` uses 8 points; a reader who
/// scrolls up that far means it.
constexpr CGFloat kPinThreshold = 8;

} // namespace

#pragma mark - The gutter

/**
 * The line numbers beside a code block, drawn rather than typed.
 *
 * A text view here would put numbers into the selection, the clipboard, and the
 * accessibility tree, which is exactly what a line number must not be. Drawing
 * them keeps copied text byte-identical with numbers on or off, and keeps a
 * screen reader from reading them.
 *
 * The gutter scrolls vertically with the code and never sideways, so the numbers
 * stay legible while a long line scrolls under them. Its width comes from the
 * digit count of the last number, so a block crossing 99 widens by one digit
 * rather than wrapping onto a second row.
 */
@interface DSCodeSegmentView ()
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
@end

@interface DSCodeGutterView : UIView
@property (nonatomic, weak, nullable) DSTextSegmentView *body;
/// Retains the owner of the line starts so mounting a gutter needs no copy.
@property (nonatomic, strong, nullable) DSCodeSegment *segment;
@property (nonatomic) uint32_t firstNumber;
@property (nonatomic) uint32_t lineCount;
@property (nonatomic) CGFloat digitAdvance;
/// The code font at this scale. Numbers are drawn in it so a gutter's digits line
/// up with the code's own monospaced columns.
@property (nonatomic, nullable, strong) UIFont *font;
/// The code's own color, which the numbers draw in at `kGutterAlpha`. Nil falls
/// back to the system secondary label, which is what a card with no theme gets.
@property (nonatomic, nullable, strong) UIColor *textColor;
@end

@implementation DSCodeGutterView {
  UIFont *_font;
  UIColor *_textColor;
}

- (instancetype)initWithFrame:(CGRect)frame {
  if (self = [super initWithFrame:frame]) {
    self.opaque = NO;
    self.backgroundColor = UIColor.clearColor;
    self.contentMode = UIViewContentModeRedraw;
    // Numbers are decoration beside the code, not text in the document, so they
    // are never a focus stop and never a VoiceOver element.
    self.isAccessibilityElement = NO;
    self.userInteractionEnabled = NO;
  }
  return self;
}

/// The font numbers are drawn in: the code font when the body has one, and a
/// digit-only monospaced face at the same size otherwise.
- (UIFont *)numberFont {
  if (self.font != nil) {
    return self.font;
  }
  return [UIFont monospacedDigitSystemFontOfSize:12 weight:UIFontWeightRegular];
}

- (void)setBody:(DSTextSegmentView *)body {
  _body = body;
  [self setNeedsDisplay];
}

- (void)setSegment:(DSCodeSegment *)segment {
  if (segment == _segment) {
    return;
  }
  _segment = segment;
  [self setNeedsDisplay];
}

- (void)setFont:(UIFont *)font {
  _font = font;
  [self setNeedsDisplay];
}

- (void)setTextColor:(UIColor *)textColor {
  _textColor = textColor;
  [self setNeedsDisplay];
}

- (void)setFirstNumber:(uint32_t)firstNumber {
  if (firstNumber == _firstNumber) {
    return;
  }
  _firstNumber = firstNumber;
  [self setNeedsDisplay];
}

- (void)traitCollectionDidChange:(UITraitCollection *)previousTraitCollection {
  [super traitCollectionDidChange:previousTraitCollection];
  if ([self.traitCollection hasDifferentColorAppearanceComparedToTraitCollection:previousTraitCollection]) {
    // The numbers take a dynamic color, which resolves against the trait
    // collection at draw time, so redrawing is all that is needed here.
    [self setNeedsDisplay];
  }
}

/// The number drawn beside logical line `index`, or 0 when the gutter has none.
- (NSString *)numberForLine:(uint32_t)index {
  if (self.segment == nil || index >= self.lineCount) {
    return nil;
  }
  return [NSString stringWithFormat:@"%u", self.firstNumber + index];
}

- (void)drawRect:(CGRect)rect {
  DSTextSegmentView *body = self.body;
  DSCodeSegment *segment = self.segment;
  if (body == nil || segment == nil || self.lineCount == 0) {
    return;
  }
  // The theme's code color dimmed, so the numbers read as an annotation on the
  // code rather than as more of it. A dynamic color resolves against the trait
  // collection here, at draw time.
  UIColor *const color = [(self.textColor ?: UIColor.secondaryLabelColor) colorWithAlphaComponent:kGutterAlpha];
  UIFont *font = self.numberFont;
  NSLayoutManager *manager = body.layoutManager;
  if (manager.numberOfGlyphs == 0) {
    return;
  }
  const CGFloat inset = body.textContainerInset.top;
  const CGFloat width = CGRectGetWidth(self.bounds);

  // Only the numbers whose lines intersect the dirty rect are drawn, so a block
  // with two thousand lines costs the same per frame as one with twenty.
  const CGRect visible = CGRectOffset(rect, 0, -inset);
  if (CGRectIsEmpty(visible)) {
    return;
  }
  // The logical line holding the character at the top of the dirty rect: one
  // glyph lookup for the point, then a search of the line starts, rather than a
  // walk over every glyph above it.
  const uint32_t *starts = segment.lineStarts->data();
  const NSUInteger topGlyph = [manager glyphIndexForPoint:CGPointMake(0, MAX(visible.origin.y, 0))
                                          inTextContainer:body.textContainer];
  const auto topCharacter = static_cast<uint32_t>([manager characterIndexForGlyphAtIndex:topGlyph]);
  const uint32_t *after = std::upper_bound(starts, starts + self.lineCount, topCharacter);
  const uint32_t first = after == starts ? 0 : static_cast<uint32_t>(after - starts - 1);
  // Each logical line starts at the first visual line its first glyph is on. A
  // wrapped continuation line gets no number, as in GitHub and VS Code.
  // A newline glyph does not have the drawn text's baseline. Find the next
  // nonblank line once from cached offsets; repeated blank lines never scan
  // the remaining text. Nonblank lines keep their own actual glyph baseline,
  // including any difference a fallback font introduces.
  NSString *const text = body.textStorage.string;
  const auto isBlank = [&](uint32_t line) {
    return starts[line] >= text.length || [text characterAtIndex:starts[line]] == '\n';
  };
  CGFloat textBaseline = -1;
  const NSUInteger baselineOffset = isBlank(first) ? [segment baselineCharacterOffsetAtOrAfter:starts[first]]
                                                  : NSNotFound;
  if (baselineOffset != NSNotFound) {
    const NSUInteger glyph =
        [manager glyphRangeForCharacterRange:NSMakeRange(baselineOffset, 0)
                       actualCharacterRange:NULL].location;
    if (glyph < manager.numberOfGlyphs) {
      textBaseline = [manager locationForGlyphAtIndex:glyph].y;
    }
  }
  NSDictionary<NSAttributedStringKey, id> *const attributes =
      @{NSFontAttributeName : font, NSForegroundColorAttributeName : color};
  for (uint32_t line = first; line < self.lineCount; line++) {
    const uint32_t offset = starts[line];
    if (offset >= manager.numberOfGlyphs && line > 0) {
      break;
    }
    const NSUInteger glyph = [manager glyphRangeForCharacterRange:NSMakeRange(offset, 0) actualCharacterRange:NULL]
                                 .location;
    if (glyph >= manager.numberOfGlyphs) {
      break;
    }
    const CGRect fragment = [manager lineFragmentRectForGlyphAtIndex:glyph effectiveRange:NULL];
    if (!isBlank(line)) {
      textBaseline = [manager locationForGlyphAtIndex:glyph].y;
    }
    const CGFloat baseline = CGRectGetMinY(fragment) + (textBaseline >= 0 ? textBaseline : font.ascender);
    if (baseline - font.ascender > CGRectGetMaxY(visible) && line > first) {
      break;
    }
    NSString *number = [self numberForLine:line];
    if (number.length == 0) {
      continue;
    }
    const CGSize size = [number sizeWithAttributes:attributes];
    // Right-aligned against the gap, so the numbers line up on their last digit
    // as they do in GitHub and VS Code.
    [number drawAtPoint:CGPointMake(std::max<CGFloat>(0, width - downstream::kGutterGap - size.width),
                                    baseline - font.ascender)
          withAttributes:attributes];
  }
}

@end

#pragma mark - The viewport

/**
 * The vertical scroll view around a capped code card's gutter and body.
 *
 * It exists on every card, not only a capped one, so a block that crosses the
 * cap mid-stream is never re-parented and keeps its selection and offsets.
 * `scrollEnabled` is what differs.
 *
 * A drag belongs to this view only while it can scroll that way. `UIScrollView`
 * has no scroll chaining, so the decision is made when the drag starts: at an
 * edge this pan recognizer declines, and the chat list's own pan takes the drag
 * from there.
 */
@implementation DSCodeViewport {
  /// A pinned card is one whose reader has not scrolled away from the bottom.
  BOOL _pinned;
  /// Set when the card asks for a pin, applied after the next layout, which is
  /// where the content size the pin needs is known.
  BOOL _needsPin;
  /// Only a touch-driven scroll unpins; a pin this view applied itself must not
  /// immediately unpin itself.
  BOOL _touchScrolling;
}

- (instancetype)initWithFrame:(CGRect)frame {
  if (self = [super initWithFrame:frame]) {
    // A capped card scrolls up and down; the horizontal scroll view inside it
    // takes the sideways drag, so only the vertical indicator is worth showing.
    self.showsHorizontalScrollIndicator = NO;
    self.showsVerticalScrollIndicator = NO;
    // No bouncing, so reaching an edge is a real edge and the hand-off is clean.
    self.alwaysBounceVertical = NO;
    self.directionalLockEnabled = YES;
    self.delaysContentTouches = NO;
    // No reader has scrolled away yet, so a block that grows past its cap while
    // streaming follows its newest line from the moment it crosses.
    _pinned = YES;
  }
  return self;
}

- (void)setStreaming:(BOOL)streaming {
  if (streaming == _streaming) {
    return;
  }
  _streaming = streaming;
  // A settled block keeps whatever offset it has, which is Streamdown's "a
  // pending pin survives the stream ending". A block that starts unsettled
  // follows its newest line, whether or not it is past its cap yet.
  if (streaming) {
    _pinned = YES;
    _needsPin = YES;
  }
}

- (void)layoutSubviews {
  [super layoutSubviews];
  if (!_needsPin) {
    return;
  }
  _needsPin = NO;
  if (_pinned) {
    [self pinToBottom];
  }
}

/// True while this card is within the threshold of its newest line, which is what
/// a reader scrolling back to the bottom restores.
- (BOOL)isNearBottom {
  const CGFloat maximum = std::max<CGFloat>(0, self.contentSize.height - CGRectGetHeight(self.bounds));
  return maximum - self.contentOffset.y <= kPinThreshold;
}

- (void)pinToBottom {
  const CGFloat maximum = std::max<CGFloat>(0, self.contentSize.height - CGRectGetHeight(self.bounds));
  if (maximum > 0) {
    // Without animation: this runs after each snapshot's layout, and an animated
    // scroll would still be moving when the next one arrives.
    [self setContentOffset:CGPointMake(0, maximum) animated:NO];
  }
}

/// Moves to the newest line after the next layout, which is where the content
/// size a pin needs is known.
- (void)requestPin {
  _needsPin = YES;
  [self setNeedsLayout];
}

#pragma mark - Drag hand-off

- (BOOL)gestureRecognizerShouldBegin:(UIGestureRecognizer *)gestureRecognizer {
  UIPanGestureRecognizer *pan = (UIPanGestureRecognizer *)gestureRecognizer;
  if (![pan isKindOfClass:UIPanGestureRecognizer.class] || !self.scrollEnabled) {
    return [super gestureRecognizerShouldBegin:gestureRecognizer];
  }
  const CGPoint velocity = [pan velocityInView:self];
  // A sideways drag belongs to the horizontal scroll view inside.
  if (std::abs(velocity.x) > std::abs(velocity.y)) {
    return NO;
  }
  const CGFloat maximum = std::max<CGFloat>(0, self.contentSize.height - CGRectGetHeight(self.bounds));
  if (maximum <= 0) {
    // Nothing below the fold: a vertical drag is the chat list's, whatever its
    // direction, which is today's behavior for an uncapped card.
    return NO;
  }
  // Already at the edge this drag would move toward: refuse, and the list's pan
  // wins. iOS decides the gesture's ownership at its start and does not hand it
  // over mid-drag, so this is the only place the decision can be made.
  if (velocity.y < 0 && self.contentOffset.y >= maximum - 0.5) {
    return NO;
  }
  if (velocity.y > 0 && self.contentOffset.y <= 0.5) {
    return NO;
  }
  return [super gestureRecognizerShouldBegin:gestureRecognizer];
}

#pragma mark - Scrolling

- (void)scrollViewDidScroll:(UIScrollView *)scrollView {
  if (!_touchScrolling) {
    return;
  }
  // More than the threshold away from the bottom and the reader has taken over.
  const CGFloat maximum = std::max<CGFloat>(0, self.contentSize.height - CGRectGetHeight(self.bounds));
  _pinned = maximum - self.contentOffset.y <= kPinThreshold;
}

- (void)scrollViewWillBeginDragging:(UIScrollView *)scrollView {
  _touchScrolling = YES;
}

- (void)scrollViewDidEndDragging:(UIScrollView *)scrollView willDecelerate:(BOOL)decelerate {
  if (!decelerate) {
    _touchScrolling = NO;
  }
}

- (void)scrollViewDidEndDecelerating:(UIScrollView *)scrollView {
  _touchScrolling = NO;
  _pinned = self.isNearBottom;
}

@end

#pragma mark - Code cards

/**
 * A code card: a fixed header row, and below it a viewport holding the line-number
 * gutter and a horizontal scroll view with a text segment view for the one code
 * block.
 *
 * The header does not scroll, so the buttons stay in place while a long line
 * scrolls, and it takes layout space rather than floating over the code, so the
 * code is never covered. The viewport exists on every card and scrolls only while
 * the block is past `codeBlockMaxHeight`, so a block crossing the cap mid-stream
 * keeps its selection and its offsets.
 */
@implementation DSCodeSegmentView {
  DSCodeSegment *_segmentStorage;
  UIView *_header;
  UILabel *_language;
  UIStackView *_buttons;
  DSCodeViewport *_viewport;
  DSCodeGutterView *_gutter;
  UIScrollView *_scrollView;
  DSTextSegmentView *_body;
  NSArray<NSString *> *_labels;
  /// The header row's text color, from the theme, which the buttons share so a
  /// themed header and its controls read as one row.
  UIColor *_headerColor;
  /// The labels the current buttons and menu items were built from, so a changed
  /// `translations` prop rebuilds them instead of leaving stale strings up.
  NSArray<NSString *> *_builtLabels;
  BOOL _copied;
  NSTimer *_copiedTimer;
  CGFloat _savedOffset;
  CGFloat _savedVerticalOffset;
  BOOL _needsScrollRestore;
  /// Set while the block is streaming, so a capped card follows its newest line.
  BOOL _streaming;
  /// Shown once, the first time this block becomes capped.
  BOOL _showedIndicator;
  /// Whether the last button pass saw a capped card, so the layout that changes
  /// the answer can rebuild the header rather than doing it twice per snapshot.
  BOOL _lastCapped;
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
    // A fence's title can be a long path, so it truncates in the middle and keeps
    // its file name rather than losing the name to an ellipsis.
    _language.lineBreakMode = NSLineBreakByTruncatingMiddle;

    _buttons = [UIStackView new];
    _buttons.axis = UILayoutConstraintAxisHorizontal;
    _buttons.spacing = 2;
    _buttons.translatesAutoresizingMaskIntoConstraints = NO;
    [_header addSubview:_language];
    [_header addSubview:_buttons];

    _viewport = [DSCodeViewport new];
    // The scroll view and the body are framed in layoutSubviews from the
    // measured body size. Under Auto Layout a non-scrolling text view would take
    // its intrinsic one-line width instead, and never wrap.
    _scrollView = [UIScrollView new];
    _scrollView.showsVerticalScrollIndicator = NO;
    _scrollView.alwaysBounceVertical = NO;
    _scrollView.directionalLockEnabled = YES;

    _body = [DSTextSegmentView new];
    _body.scrollEnabled = NO;

    _gutter = [DSCodeGutterView new];

    [_viewport addSubview:_gutter];
    [_scrollView addSubview:_body];
    [_viewport addSubview:_scrollView];
    [self addSubview:_header];
    [self addSubview:_viewport];
    [self setConstraints];
  }
  return self;
}

- (void)setConstraints {
  [NSLayoutConstraint activateConstraints:@[
    [_header.topAnchor constraintEqualToAnchor:self.topAnchor],
    [_header.leadingAnchor constraintEqualToAnchor:self.leadingAnchor],
    [_header.trailingAnchor constraintEqualToAnchor:self.trailingAnchor],
    [_header.heightAnchor constraintEqualToConstant:self.showsHeader ? kChromeRowHeight : 0],
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

- (DSCodeSegment *)segment {
  return _segmentStorage;
}

- (void)setHosted:(BOOL)hosted {
  if (_hosted == hosted) {
    return;
  }
  _hosted = hosted;
  // The header, the body and the gutter all belong to the card, which is the
  // fallback: a hosted block shows none of them, because the React child above
  // this view is what the reader sees. Hiding them rather than skipping the draw
  // also means the view has no text of its own to select.
  _header.hidden = hosted;
  _viewport.hidden = hosted;
  _gutter.hidden = hosted;
  if (!hosted) {
    [self setNeedsDisplay];
  }
}

- (void)setSegment:(DSCodeSegment *)segment {
  if (segment == _segmentStorage) {
    return;
  }
  // The tail block is rebuilt on every snapshot while its fence is open, so both
  // offsets are kept only while the block still looks like the same one.
  const BOOL sameBlock = _segmentStorage != nil && segment != nil &&
                         _segmentStorage.codeOrdinal == segment.codeOrdinal &&
                         [_segmentStorage.firstLine isEqualToString:segment.firstLine];
  _savedOffset = sameBlock ? _scrollView.contentOffset.x : 0;
  _savedVerticalOffset = sameBlock ? _viewport.contentOffset.y : 0;
  _needsScrollRestore = YES;
  // A new block arrives unsettled, so a capped card starts pinned to its newest
  // line. `setStreaming:` turns that off once the fence closes.
  _streaming = YES;
  _segmentStorage = segment;
  _body.linkDelegate = self.linkDelegate;
  [_body setSegment:segment.text];
  _gutter.body = _body;
  _gutter.segment = segment;
  _gutter.firstNumber = segment.fenceMeta.startLine;
  _gutter.lineCount = segment.lineCount;
  _gutter.digitAdvance = segment.digitAdvance;
  _gutter.font = segment.codeFont;
  // The gutter draws in the code's own color, dimmed rather than re-themed: the
  // numbers annotate the code beside them, so a theme that restyles code restyles
  // its numbers in step. Streamdown's `muted-foreground/50` is the same idea.
  _gutter.textColor = [[segment.theme colorForSlot:kSlotCodeBlockColor
                                            fallback:UIColor.secondaryLabelColor]
      colorWithAlphaComponent:kGutterAlpha];
  // A fence's title replaces the language, and the spoken label keeps both plus
  // the start line, so a screen reader still hears what kind of code this is.
  _language.text = segment.title.length > 0 ? segment.title : segment.language;
  _language.accessibilityLabel = [self spokenLabel];
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

/// "src/app.ts, ts code, from line 10": both names, and the start line when it
/// is not the default. The numbers themselves are never read aloud.
- (NSString *)spokenLabel {
  const DSCodeSegment *segment = _segmentStorage;
  NSString *language = segment.language;
  NSString *title = segment.title;
  const BOOL hasTitle = title.length > 0 && ![title isEqualToString:language];
  NSMutableArray<NSString *> *parts = [NSMutableArray new];
  if (hasTitle) {
    [parts addObject:title];
  }
  if (language.length > 0) {
    [parts addObject:[NSString stringWithFormat:@"%@ code", language]];
  } else {
    [parts addObject:@"code"];
  }
  if (segment.fenceMeta.startLine != 1) {
    [parts addObject:[NSString stringWithFormat:@"from line %u", segment.fenceMeta.startLine]];
  }
  return [parts componentsJoinedByString:@", "];
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

- (void)setLayout:(downstream::CodeBlockLayout)layout {
  if (layout.wraps == _layout.wraps && layout.lineNumbers == _layout.lineNumbers &&
      layout.firstNumber == _layout.firstNumber && layout.maxHeight == _layout.maxHeight &&
      layout.expanded == _layout.expanded) {
    return;
  }
  _layout = layout;
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
  UIButton *expand = makeControlButton(symbolForExpand(NO), [self label:DSControlLabelExpandCode], @"expand", @"");
  UIButton *wrap = makeControlButton(symbolForWrap(NO), [self label:DSControlLabelWrapCode], @"wrap", @"");
  UIButton *download =
      makeControlButton(kSymbolDownload, [self label:DSControlLabelDownloadFile], @"download", @"text");
  UIButton *copy = makeControlButton(kSymbolCopy, [self label:DSControlLabelCopyCode], @"copy", @"text");
  // A fixed order, so the expand button does not shift the others as it appears
  // and disappears with a block's height.
  expand.tag = 0;
  wrap.tag = 1;
  download.tag = 2;
  copy.tag = 3;
  for (UIButton *button in @[expand, wrap, download, copy]) {
    button.accessibilityIdentifier = [NSString stringWithFormat:@"ds.code.%@", button.accessibilityLabel];
    [button addTarget:self action:@selector(handleControl:) forControlEvents:UIControlEventTouchUpInside];
    [_buttons addArrangedSubview:button];
  }
}

/// Rebuilds the header's buttons from the current controls, enabled state, and
/// layout. The buttons are created once; only their glyphs, labels, and states
/// change, which is what keeps a snapshot from allocating any.
/// Rebuilds the header's buttons from the current controls, enabled state, and
/// layout. The buttons are created once; only their glyphs, labels, and states
/// change, which is what keeps a snapshot from allocating any.
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
  // Expand appears only on a block past the cap, because that is the only block
  // it can do anything for, and a hidden button is cheaper than a wrong one. An
  // expanded block keeps it as a collapse button, so the cap can come back.
  const BOOL capped = [self isCapped];
  _lastCapped = capped;
  const BOOL expanded = _layout.expanded;
  const BOOL showExpand = (_controls & CodeExpand) != 0 && (capped || expanded);
  const BOOL showWrap = (_controls & CodeWrap) != 0;
  ((UIButton *)all[0]).hidden = !showExpand;
  ((UIButton *)all[1]).hidden = !showWrap;
  ((UIButton *)all[2]).hidden = (_controls & CodeDownload) == 0;
  ((UIButton *)all[3]).hidden = (_controls & CodeCopy) == 0;
  for (UIButton *button in all) {
    button.enabled = _controlsEnabled && !button.hidden;
  }

  UIButton *expand = (UIButton *)all[0];
  expand.selected = expanded;
  [expand setImage:controlImage(symbolForExpand(expanded)) forState:UIControlStateNormal];
  expand.accessibilityLabel = expanded ? [self label:DSControlLabelCollapseCode]
                                       : [self label:DSControlLabelExpandCode];
  // No explicit traits: UIButton derives Selected and Not Enabled itself, and a
  // fixed value would tell VoiceOver a disabled button can be pressed.

  UIButton *wrap = (UIButton *)all[1];
  wrap.selected = _layout.wraps;
  [wrap setImage:controlImage(symbolForWrap(_layout.wraps)) forState:UIControlStateNormal];
  wrap.accessibilityLabel = _layout.wraps ? [self label:DSControlLabelUnwrapCode] : [self label:DSControlLabelWrapCode];

  UIButton *download = (UIButton *)all[2];
  download.accessibilityLabel = [self label:DSControlLabelDownloadFile];
  UIButton *copy = (UIButton *)all[3];
  [copy setImage:controlImage(symbolForCopy(_copied)) forState:UIControlStateNormal];
  copy.accessibilityLabel = _copied ? [self label:DSControlLabelCopied] : [self label:DSControlLabelCopyCode];
  [self updateCopiedSpokenText];
}

/// True when this block's body is taller than its cap, which is what makes the
/// viewport scroll and the expand button mean something.
- (BOOL)isCapped {
  if (_segmentStorage == nil || !(_layout.maxHeight > 0)) {
    return NO;
  }
  return _viewport.contentSize.height > _layout.maxHeight;
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
  if (!sender.enabled || _segmentStorage == nil) {
    return;
  }
  if ([button.action isEqualToString:@"wrap"]) {
    [self.controlDelegate didToggleWrapAtOrdinal:_segmentStorage.codeOrdinal
                                        language:_segmentStorage.language
                                         wrapped:!_layout.wraps];
    return;
  }
  if ([button.action isEqualToString:@"expand"]) {
    [self.controlDelegate didToggleExpandAtOrdinal:_segmentStorage.codeOrdinal
                                          language:_segmentStorage.language
                                          expanded:!_layout.expanded];
    return;
  }
  if ([button.action isEqualToString:@"copy"]) {
    [self markCopied];
  }
  [self.controlDelegate didPressControlOfKind:DSBlockKindCode
                                   blockIndex:_segmentStorage.blockIndex
                                   blockCount:1
                                     language:_segmentStorage.language
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

/// Drops the saved offsets and returns to the top, for a new generation.
- (void)resetScrollPosition {
  _needsScrollRestore = YES;
  _savedOffset = 0;
  _savedVerticalOffset = 0;
  _showedIndicator = NO;
  [self setNeedsLayout];
}

- (void)layoutSubviews {
  [super layoutSubviews];
  if (_segmentStorage == nil) {
    return;
  }
  const CGFloat headerHeight = self.showsHeader ? kChromeRowHeight : 0;
  // The card's padding is the theme's, so a theme that pads a card wider gives the
  // body and the gutter exactly that much less room.
  const CGFloat leading = _segmentStorage.contentLeading;
  const CGFloat padding = _segmentStorage.padding;
  const CGRect card = CGRectMake(leading, headerHeight, self.bounds.size.width - leading,
                                 self.bounds.size.height - headerHeight);
  const CGFloat gutterWidth = [_segmentStorage gutterWidthForLayout:_layout];
  _gutter.hidden = gutterWidth <= 0;
  // The gutter narrows the width a wrapped body wraps at, which is the only way
  // numbers change a block's height. An unwrapped body is laid out at its own
  // length either way, so it never lays code out again on a rotation.
  const CGFloat bodyWidth =
      [_segmentStorage.text usedWidthForWidth:[_segmentStorage layoutWidthForCard:card.size.width
                                                                         wrapped:_layout.wraps
                                                                    gutterWidth:gutterWidth]];
  const CGFloat bodyHeight =
      [_segmentStorage.text heightForWidth:[_segmentStorage layoutWidthForCard:card.size.width
                                                                      wrapped:_layout.wraps
                                                                 gutterWidth:gutterWidth]];
  const CGRect viewport = CGRectInset(card, padding, padding);
  _viewport.frame = viewport;
  // The gutter sits inside the card's padding, at the leading edge, and the
  // horizontal scroll view takes the rest.
  _gutter.frame = CGRectMake(0, 0, gutterWidth, std::max<CGFloat>(bodyHeight, viewport.size.height));
  _scrollView.frame = CGRectMake(gutterWidth, 0, std::max<CGFloat>(0, viewport.size.width - gutterWidth),
                                 _gutter.frame.size.height);
  _body.frame = CGRectMake(0, 0, std::max<CGFloat>(bodyWidth, 0), std::max<CGFloat>(bodyHeight, 0));
  _scrollView.contentSize = _body.bounds.size;
  // A gutter is as tall as the code, and it scrolls vertically with it, so the
  // numbers beside the visible lines are the ones the reader sees.
  _viewport.contentSize = CGSizeMake(viewport.size.width, _gutter.frame.size.height);
  // Whether the block is capped is only known here, and the expand button depends
  // on it, so this is where a header that appears or disappears is decided. The
  // header row's height is fixed either way, so nothing moves.
  if ([self isCapped] != _lastCapped) {
    [self updateButtons];
  }

  // Vertical scrolling turns on only while the block is past its cap, which is
  // the whole difference between a capped card and an uncapped one.
  // The viewport scrolls only while the block is past its cap, which is the whole
  // difference between a capped card and an uncapped one.
  const BOOL capped = _layout.maxHeight > 0 && bodyHeight + 2 * padding > _layout.maxHeight;
  _viewport.scrollEnabled = capped;
  _viewport.streaming = _streaming;
  if (capped && !_showedIndicator) {
    // Flash the indicator the first time a block becomes capped, so the scroll is
    // discoverable rather than merely available.
    _showedIndicator = YES;
    [_viewport flashScrollIndicators];
  }

  if (_needsScrollRestore) {
    const CGFloat maximum = std::max<CGFloat>(0, _scrollView.contentSize.width - _scrollView.bounds.size.width);
    _scrollView.contentOffset = CGPointMake(std::clamp(_savedOffset, 0.0, maximum), 0);
    const CGFloat verticalMaximum = std::max<CGFloat>(0, _viewport.contentSize.height - viewport.size.height);
    _viewport.contentOffset = CGPointMake(0, std::clamp(_savedVerticalOffset, 0.0, verticalMaximum));
    _needsScrollRestore = NO;
  }
  // Follow the newest line, which is Streamdown's pinned scroll: after each
  // snapshot's layout, a pinned card goes to the bottom. Not an alternative to
  // the restore above: an open fence is rebuilt on every snapshot, so the restore
  // runs every time, and the pin has to land on top of it.
  if (capped && _streaming) {
    [_viewport requestPin];
  }
  [self setNeedsDisplay];
}

/// The card's rounded code background, plus the container bars and the list
/// marker, which live in the non-scrolling margin beside it, and a bottom fade
/// while a capped block has more below.
- (void)drawRect:(CGRect)rect {
  if (_segmentStorage == nil) {
    return;
  }
  const CGFloat leading = _segmentStorage.contentLeading;
  // A host segment has no header, so its chrome starts at the top of its own
  // frame and spans the height the React component measured.
  const CGFloat headerHeight = self.hosted ? 0 : (self.showsHeader ? kChromeRowHeight : 0);
  if (!self.hosted) {
    // A rule under the header row, when the theme asked for one. Its color
    // defaults to clear, so a theme that says nothing draws nothing.
    UIColor *headerBorder = _segmentStorage.headerBorderColor;
    if (headerBorder != nil && self.showsHeader) {
      [headerBorder setFill];
      UIRectFill(CGRectMake(0, headerHeight - 1, self.bounds.size.width, 1));
    }
    const CGRect card = CGRectMake(leading, headerHeight, self.bounds.size.width - leading,
                                   self.bounds.size.height - headerHeight);
    // The card's fill, radius and border are all the theme's, and the border is
    // drawn rather than filled because it sits inside the card's bounds, which is
    // what keeps a themed card the height the measurement gave it.
    UIColor *fill = [_segmentStorage.theme colorForSlot:kSlotCodeBlockBackgroundColor
                                             fallback:UIColor.tertiarySystemFillColor];
    UIBezierPath *path = [UIBezierPath bezierPathWithRoundedRect:card cornerRadius:_segmentStorage.borderRadius];
    [fill setFill];
    [path fill];
    if (_segmentStorage.borderWidth > 0) {
      [[_segmentStorage.theme colorForSlot:kSlotCodeBlockBorderColor fallback:nil] setStroke];
      path.lineWidth = _segmentStorage.borderWidth;
      [path stroke];
    }
  }

  for (DSContainerInfo *container in _segmentStorage.containers) {
    if (container.barColor != nil && container.barWidth > 0) {
      [container.barColor setFill];
      // Mirrored for a right-to-left block, the same way the marker is.
      const CGFloat x = _segmentStorage.rightToLeft
                            ? self.bounds.size.width - container.leading - container.barWidth - 2
                            : container.leading + 2;
      UIRectFill(CGRectMake(x, headerHeight, container.barWidth, self.bounds.size.height - headerHeight));
    }
    if (container.marker != nil) {
      UIFont *font = [container.marker attribute:NSFontAttributeName atIndex:0 effectiveRange:NULL];
      // A hosted block's marker sits at the top of its own height, beside the
      // component, rather than beside the first line of code that is not there.
      const CGFloat top = self.hosted ? 0 : _segmentStorage.padding;
      const CGFloat baseline = headerHeight + top + font.ascender;
      const CGFloat x = _segmentStorage.rightToLeft
                            ? container.leading + container.markerGap
                            : container.leading + container.width - container.markerGap -
                                  container.marker.size.width;
      [container.marker drawAtPoint:CGPointMake(x, baseline - font.ascender)];
    }
  }
  if (self.hosted) {
    // Nothing else: a hosted block has no card to fade, no header row and no
    // scrolling viewport.
    return;
  }

  // The fade says there is more below without covering the last line's text: it
  // is short, and it sits inside the card's own bottom padding.
  const CGRect card = CGRectMake(leading, headerHeight, self.bounds.size.width - leading,
                                 self.bounds.size.height - headerHeight);
  if (_viewport.scrollEnabled && _viewport.contentOffset.y < _viewport.contentSize.height - CGRectGetHeight(_viewport.bounds) - 0.5) {
    const CGFloat height = std::min<CGFloat>(kBottomFade, CGRectGetHeight(card) / 2);
    CGContextRef context = UIGraphicsGetCurrentContext();
    CGContextSaveGState(context);
    [[UIBezierPath bezierPathWithRoundedRect:card cornerRadius:6] addClip];
    // The card's own fill, fading to nothing, so the fade reads as more of the
    // card rather than as a gray bar over it. The dynamic color resolves against
    // the trait collection here, in the draw pass, so this is the appearance's
    // fill rather than a fixed one.
    CGFloat red = 0;
    CGFloat green = 0;
    CGFloat blue = 0;
    CGFloat alpha = 0;
    if ([UIColor.tertiarySystemFillColor getRed:&red green:&green blue:&blue alpha:&alpha]) {
      CGFloat components[8] = {red, green, blue, alpha, red, green, blue, 0};
      const CGFloat locations[2] = {0, 1};
      CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
      CGGradientRef gradient = CGGradientCreateWithColorComponents(space, components, locations, 2);
      if (gradient != NULL) {
        CGContextDrawLinearGradient(context, gradient, CGPointMake(CGRectGetMinX(card), CGRectGetMaxY(card) - height),
                                    CGPointMake(CGRectGetMinX(card), CGRectGetMaxY(card)), 0);
        CGGradientRelease(gradient);
      }
      CGColorSpaceRelease(space);
    }
    CGContextRestoreGState(context);
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
  // Streamdown's DOM reads in. The gutter is not listed: it holds no text.
  if (_segmentStorage == nil) {
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
  // in the body's space, which the header and the scroll offsets move.
  [elements addObject:_body];
  return elements;
}

/// The block is finished once its fence has closed. A card stops following its
/// newest line then, and keeps whatever offset the reader left.
- (void)setStreaming:(BOOL)streaming {
  if (streaming == _streaming) {
    return;
  }
  _streaming = streaming;
  [self setNeedsLayout];
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
  /// A code block's ordinals, so a wrap or expand choice can be applied by
  /// ordinal rather than by segment index, which moves as blocks stream in.
  NSMutableArray<NSNumber *> *_codeOrdinals;
  /// The whole code layout, which both the card views and the frame stack read,
  /// so a block's drawn layout and its measured height are the same decision.
  downstream::CodeLayout _codeLayout;
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
    ((DSTextSegmentView *)view).selectable = _selectable && [self isSegmentSettledAtIndex:index];
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
                                                              hosts:_hostLayout];
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
