#import "DSSegmentViews.h"

#import "DSLayoutManager.h"
#import "DSRenderedDocument.h"

#include "Controls.h"

#include <algorithm>
#include <cmath>

using namespace downstream;
using namespace downstream::ControlBits;
// The slot names a themed drawing reads. The layout helpers they sit beside live
// in `theme_layout`, which needs no `using` here: the metrics arrive on the
// segment rather than as a rule to apply.
using namespace downstream::theme;

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

/// How far a line number is dimmed below the code it numbers, which is what
/// Streamdown's `muted-foreground/50` gutter draws at. Applied to the theme's own
/// code color rather than to a fixed gray, so a theme that restyles code restyles
/// its numbers in step.
constexpr CGFloat kGutterAlpha = 0.5;

/// How long a copy button shows its check before going back, as in Streamdown.
constexpr NSTimeInterval kCopiedDuration = 2.0;

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
/// The document block the card's body starts at, so a gutter can ask the fade
/// for its block's alpha rather than for its body's.
@property (nonatomic) NSInteger blockOffset;
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

/// The line bands a fence asked for with `{1,3-5}` in its info string.
///
/// Drawn behind the code rather than beside it, and behind *every* visual line a
/// logical line wraps onto, because a mark is about a line of the file rather
/// than about a line of the screen: marking only the first visual row would leave
/// the rest of a wrapped marked line looking unmarked.
///
/// No height of its own, so it is not a layout input and does not join
/// `LayoutHint` — the bands are drawn into the card's existing rows.
- (void)drawLineMarksInRect:(CGRect)rect {
  DSCodeSegment *segment = self.segment;
  DSTextSegmentView *body = self.body;
  const std::vector<LineMark> &marks = segment.fenceMeta.marks;
  if (marks.empty() || body == nil || self.lineCount == 0) {
    return;
  }
  NSLayoutManager *manager = body.layoutManager;
  UIColor *const color = [segment.theme colorForSlot:kSlotCodeBlockLineMarkColor fallback:UIColor.clearColor];
  if (color == nil || CGColorGetAlpha(color.CGColor) == 0) {
    return;
  }
  const CGFloat inset = body.textContainerInset.top;
  const uint32_t *starts = segment.lineStarts->data();
  const uint32_t firstNumber = segment.fenceMeta.startLine;
  [color setFill];
  for (const LineMark &mark : marks) {
    // Marked lines are the fence's own numbering, so a fence that started at line
    // 10 marks line 10 first — which is what makes `{1,3-5}` on a diff mean the
    // same thing as it does in every other diff viewer.
    for (int64_t index = static_cast<int64_t>(mark.first) - firstNumber;
         index <= static_cast<int64_t>(mark.last) - firstNumber; index++) {
      if (index < 0 || index >= self.lineCount) {
        continue;
      }
      const uint32_t offset = starts[index];
      if (offset >= body.textStorage.length) {
        continue;
      }
      NSRange line;
      // The effective range is the whole line, which is what finds a wrapped
      // line's continuation rows as well as the one it starts on.
      const CGRect fragment = [manager lineFragmentRectForGlyphAtIndex:[manager glyphIndexForCharacterAtIndex:offset]
                                                       effectiveRange:&line];
      for (NSUInteger glyph = line.location; glyph < NSMaxRange(line); glyph++) {
        const CGRect row = [manager lineFragmentRectForGlyphAtIndex:glyph effectiveRange:NULL];
        const CGRect drawn = CGRectOffset(row, 0, inset);
        if (CGRectGetMinY(drawn) > CGRectGetMaxY(rect)) {
          break;
        }
        if (CGRectGetMaxY(drawn) < CGRectGetMinY(rect)) {
          continue;
        }
        UIRectFill(CGRectMake(0, drawn.origin.y, CGRectGetWidth(self.bounds), drawn.size.height));
      }
      (void)fragment;
    }
  }
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
  [self drawLineMarksInRect:CGRectOffset(rect, 0, -inset)];

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
    // A line's number fades with that line's own text, which is what makes a new
    // line of streaming code bring its number with it rather than showing the
    // number first.
    const id<DSRevealProvider> provider = body.revealProvider;
    const CGFloat alpha =
        provider == nil
            ? 1
            : [provider revealAlphaAtBlock:self.blockOffset offset:offset now:CACurrentMediaTime()];
    if (alpha <= 0) {
      continue;
    }
    // Right-aligned against the gap, so the numbers line up on their last digit
    // as they do in GitHub and VS Code.
    [number drawAtPoint:CGPointMake(std::max<CGFloat>(0, width - downstream::kGutterGap - size.width),
                                    baseline - font.ascender)
          withAttributes:alpha >= 1 ? attributes : @{
            NSFontAttributeName : font,
            NSForegroundColorAttributeName : [color colorWithAlphaComponent:CGColorGetAlpha(color.CGColor) * alpha]
          }];
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
  NSArray<NSString *> *_icons;
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
  /// The document block this card's body starts at, which a reveal unit's own
  /// block index is relative to.
  NSInteger _blockOffset;
  /// Where the body and the gutter read their fading alphas from. Weak, because
  /// the provider is the Markdown view's and this card is recycled.
  __weak id<DSRevealProvider> _revealProvider;
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
    // Hidden until the chrome asks for it, the same rule `setShowsHeader:` and
    // `setHosted:` apply, so the zero-height row never shows its label.
    _header.hidden = !_showsHeader;
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

- (void)setControlIcons:(NSArray<NSString *> *)icons {
  _icons = [icons copy];
  [self updateButtons];
}

/// The glyph for one key, or the platform's default for an entry the app did not
/// name.
///
/// The name is resolved here rather than in JavaScript because a symbol name is
/// platform-specific: the same `download` key is an SF Symbol on iOS and a
/// drawable on Android, and neither is the other's spelling.
- (NSString *)icon:(NSString *)key fallback:(NSString *)fallback {
  NSUInteger index = [self indexOfIcon:key];
  NSString *const named = index < _icons.count ? _icons[index] : @"";
  return named.length > 0 ? named : fallback;
}

- (NSUInteger)indexOfIcon:(NSString *)key {
  static NSArray<NSString *> *keys;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    keys = @[
      DSControlIconCopy, DSControlIconCheck, DSControlIconDownload, DSControlIconMaximize,
      DSControlIconClose, DSControlIconWrap, DSControlIconExpand, DSControlIconCollapse,
      DSControlIconTableFullscreen, DSControlIconDetailsChevron,
    ];
  });
  return [keys indexOfObject:key];
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
  // also means the view has no text of its own to select. The header follows
  // both inputs, because the chrome sets `showsHeader` after this on every
  // render and must not bring a card's title row back above a chart.
  _header.hidden = !_showsHeader || hosted;
  _viewport.hidden = hosted;
  _gutter.hidden = hosted;
  if (!hosted) {
    [self setNeedsDisplay];
  }
}

/// The document block this card's body starts at. Set on every render because a
/// card is reused across snapshots and the block it holds moves each time.
- (void)setBlockOffset:(NSInteger)blockOffset {
  if (_blockOffset == blockOffset) {
    return;
  }
  _blockOffset = blockOffset;
  _body.blockOffset = blockOffset;
  _gutter.blockOffset = blockOffset;
  [_gutter setNeedsDisplay];
}

- (void)setRevealProvider:(id<DSRevealProvider>)provider {
  if (_revealProvider == provider) {
    return;
  }
  _revealProvider = provider;
  _body.revealProvider = provider;
  [_gutter setNeedsDisplay];
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
  _body.blockOffset = _blockOffset;
  _gutter.body = _body;
  _gutter.segment = segment;
  _gutter.firstNumber = segment.fenceMeta.startLine;
  _gutter.lineCount = segment.lineCount;
  _gutter.digitAdvance = segment.digitAdvance;
  _gutter.font = segment.codeFont;
  // The gutter's line starts are the same offsets a reveal unit names, so asking
  // for the block's own alpha per line is a lookup rather than a mapping.
  _gutter.blockOffset = _blockOffset;
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
  _header.hidden = !_showsHeader || _hosted;
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
  UIButton *expand = makeControlButton(
      [self icon:DSControlIconExpand fallback:symbolForExpand(NO)], [self label:DSControlLabelExpandCode],
      @"expand", @"");
  UIButton *wrap = makeControlButton(
      [self icon:DSControlIconWrap fallback:symbolForWrap(NO)], [self label:DSControlLabelWrapCode], @"wrap", @"");
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
  [expand setImage:controlImage([self icon:(expanded ? DSControlIconCollapse : DSControlIconExpand)
                                    fallback:symbolForExpand(expanded)])
            forState:UIControlStateNormal];
  expand.accessibilityLabel = expanded ? [self label:DSControlLabelCollapseCode]
                                       : [self label:DSControlLabelExpandCode];
  // No explicit traits: UIButton derives Selected and Not Enabled itself, and a
  // fixed value would tell VoiceOver a disabled button can be pressed.

  UIButton *wrap = (UIButton *)all[1];
  wrap.selected = _layout.wraps;
  [wrap setImage:controlImage([self icon:(_layout.wraps ? DSControlIconCollapse : DSControlIconWrap)
                                   fallback:symbolForWrap(_layout.wraps)])
           forState:UIControlStateNormal];
  wrap.accessibilityLabel = _layout.wraps ? [self label:DSControlLabelUnwrapCode] : [self label:DSControlLabelWrapCode];

  UIButton *download = (UIButton *)all[2];
  download.accessibilityLabel = [self label:DSControlLabelDownloadFile];
  UIButton *copy = (UIButton *)all[3];
  [copy setImage:controlImage([self icon:(_copied ? DSControlIconCheck : DSControlIconCopy)
                                  fallback:symbolForCopy(_copied)])
           forState:UIControlStateNormal];
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
  // A card fades with its own first unit, so a new code block brings its
  // background, its border, and its header with it rather than appearing behind
  // text that is already there. Its body fades by its own units, so the card does
  // not multiply them. A hosted block does not fade at all: the React child over
  // it is not faded by the reveal, so its bars and marker arrive with the
  // component rather than with code text the reader never sees.
  const CGFloat alpha = _revealProvider == nil || self.hosted
      ? 1
      : [_revealProvider revealAlphaAtBlock:_blockOffset offset:0 now:CACurrentMediaTime()];
  CGContextRef alphaContext = UIGraphicsGetCurrentContext();
  CGContextSaveGState(alphaContext);
  CGContextSetAlpha(alphaContext, alpha);
  // A hosted fence draws no card: the React child mounted over this view is what
  // the reader sees, and a card behind it would be a second answer to the same
  // block.
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
    if (container.marker.length > 0) {
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
  CGContextRestoreGState(alphaContext);
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

/// A card's chrome fades through the card's own alpha, its numbers through the
/// gutter's, and its code through the body's text. Each is a view of its own
/// that draws only when asked, so the card, the gutter and the body's fading
/// lines are all invalidated.
- (void)invalidateRevealCharacterRange:(NSRange)range
                            blockOffset:(NSInteger)blockOffset
                                     now:(double)now {
  [self setNeedsDisplay];
  [_gutter setNeedsDisplay];
  [_body invalidateRevealCharacterRange:range blockOffset:blockOffset now:now];
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
