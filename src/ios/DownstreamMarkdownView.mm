#import "DownstreamMarkdownView.h"

#import "DSFileExporter.h"
#import "DSRenderedDocument.h"
#import "DSSegmentViews.h"
#import "DSThemeColors.h"
#import <QuartzCore/CADisplayLink.h>

#import <React/RCTConvert.h>
#import <react/renderer/components/DownstreamSpec/EventEmitters.h>
#import <react/renderer/components/DownstreamSpec/Props.h>
#import <react/renderer/components/DownstreamSpec/RCTComponentViewHelpers.h>

#include "Caret.h"
#include "Controls.h"
#include "DownstreamMarkdownComponentDescriptor.h"
#include "Export.h"
#include "Reveal.h"
#include "SegmentPlan.h"
#include "TableLayout.h"
#include "theme/Theme.h"
#include "SessionTrace.h"
#include "WorkerPool.h"

#include <algorithm>
#include <exception>
#include <string>
#include <vector>

using namespace facebook::react;
using namespace downstream;

namespace {

/// Larger exports serialize on the worker pool. Include per-cell overhead in
/// the estimate so even a large table of empty cells leaves the main thread.
constexpr size_t kInlineExportBytes = 64 * 1024;
/// Clipboard writes complete on the main thread. A newer copy press, even in
/// another Markdown view, supersedes an older worker that is still running.
uint64_t copyRequestEpoch = 0;

std::span<const downstream::RevealUnit> unitsForLayout(
    std::span<const downstream::RevealUnit> units, NSInteger blockOffset, NSUInteger blockCount) {
  if (blockOffset < 0 || blockCount == 0 || units.empty()) {
    return {};
  }
  const uint64_t firstBlock = static_cast<uint64_t>(blockOffset);
  const uint64_t lastBlock = firstBlock + blockCount;
  const auto first = std::lower_bound(units.begin(), units.end(), firstBlock,
      [](const downstream::RevealUnit &unit, uint64_t block) { return unit.block < block; });
  const auto last = std::lower_bound(first, units.end(), lastBlock,
      [](const downstream::RevealUnit &unit, uint64_t block) { return unit.block < block; });
  return units.subspan(static_cast<size_t>(first - units.begin()), static_cast<size_t>(last - first));
}

NSString *toNSString(const std::u16string &text) {
  return [NSString stringWithCharacters:reinterpret_cast<const unichar *>(text.data()) length:text.size()];
}

NSString *toNSString(const std::string &text) {
  return [NSString stringWithUTF8String:text.c_str()] ?: @"";
}

DSExportFormat formatFor(NSString *format) {
  if ([format isEqualToString:@"csv"]) {
    return DSExportFormatCsv;
  }
  if ([format isEqualToString:@"tsv"]) {
    return DSExportFormatTsv;
  }
  if ([format isEqualToString:@"markdown"]) {
    return DSExportFormatMarkdown;
  }
  if ([format isEqualToString:@"html"]) {
    return DSExportFormatHtml;
  }
  return DSExportFormatText;
}

/// A CSV separator the locale chooses: a comma unless this locale writes
/// decimals with one, which is what Streamdown's `auto` does in the browser.
char16_t resolveSeparator(NSString *configured) {
  if ([configured isEqualToString:@";"]) {
    return u';';
  }
  if ([configured isEqualToString:@"\\t"] || [configured isEqualToString:@"\t"]) {
    return u'\t';
  }
  if (![configured isEqualToString:@"auto"]) {
    return u',';
  }
  return [NSLocale.currentLocale.decimalSeparator isEqualToString:@","] ? u';' : u',';
}

/// Whether `element` sits above `view`, treating an element with no frame of
/// its own as the start of the message.
bool frameIsAbove(CGRect element, UIView *view) {
  if (CGRectIsEmpty(element)) {
    return false;
  }
  return CGRectGetMaxY(element) <= CGRectGetMinY(view.frame);
}

NSString *nameForFormat(NSString *format) {
  if ([format isEqualToString:@"csv"]) {
    return @"csv";
  }
  if ([format isEqualToString:@"tsv"]) {
    return @"tsv";
  }
  if ([format isEqualToString:@"markdown"]) {
    return @"md";
  }
  return @"txt";
}

} // namespace

@interface DownstreamMarkdownView () <RCTDownstreamMarkdownViewViewProtocol, DSLinkDelegate, DSControlDelegate,
                                     DSRevealProvider, DSImageDelegate>
- (void)scheduleTraceFrame;
- (void)applyRendering;
- (void)applyReveal;
- (void)stopReveal;
- (void)finishReveal;
@end

@implementation DownstreamMarkdownView {
  DSDocumentView *_documentView;
  /// Where a hosted React child lives: a plain container above the document view
  /// that neither draws nor lays anything out, so Fabric's frame is the child's
  /// own and the document's own children are untouched.
  UIView *_hostContainer;
  DSFileExporter *_exporter;
  DownstreamMarkdownShadowNode::ConcreteState::Shared _state;
  downstream::DocumentHandle _document;
  downstream::Theme::Handle _theme;
  /// The theme's id, which is what says whether the theme actually changed.
  uint64_t _themeId;
  CGFloat _fontScale;
  BOOL _syntaxHighlighting;
  BOOL _selectable;
  uint32_t _controls;
  uint32_t _chrome;
  downstream::CodeLayout _codeLayout;
  /// What each image in the document is, which the views draw their boxes from
  /// and the frames are stacked with. A load changes an entry, and that is the
  /// whole path from a picture arriving to the document being taller.
  downstream::ImageStateLayout _imageState;
  /// Which fenced blocks a React child replaced, and how tall each one is. Read
  /// from the state beside the image answers, because both are properties of
  /// this view at this moment rather than of the document.
  downstream::HostLayout _hostLayout;
  /// `tableMaxHeight` in points, 0 for no cap. A height rather than a prop read
  /// from the segment, because a control press resolves one table's own layout
  /// and a segment view has no access to the view's props.
  downstream::TableLayout _tableLayout;
  /// The reader's one set of choices, which the two layouts above both carry.
  /// Held beside them so a details toggle can be answered from the state without
  /// either layout being the place it lives.
  downstream::ReaderChoices _choices;
  std::string _codeFilename;
  std::string _tableFilename;
  std::string _imageFilename;
  /// The image whose button was pressed, so the control event can name the
  /// destination the way a press does. Held only between the two calls a save
  /// makes, and cleared on a recycle.
  NSString *_pressedImageURL;
  NSString *_csvSeparator;
  NSArray<NSString *> *_controlLabels;
  /// The glyph names the `icons` prop resolved to, in `CONTROL_ICON_KEYS` order.
  /// An empty string is a key the app removed, which the button reads as "draw no
  /// icon" — a text-only button is how a quiet header is built.
  NSArray<NSString *> *_controlIcons;
  BOOL _linkPreview;
  /// Per-URL link styles, as four flat arrays: every prefix, the counts that say
  /// where one variant's prefixes end, a processed color per variant or -1, and
  /// whether it underlines.
  NSArray<NSString *> *_linkVariantPrefixes;
  NSArray<NSNumber *> *_linkVariantCounts;
  NSArray<NSNumber *> *_linkVariantColors;
  NSArray<NSNumber *> *_linkVariantUnderlines;
  CADisplayLink *_traceDisplayLink;
  uint64_t _traceDocumentId;
  uint64_t _controlEpoch;
  /// What is still fading in this view. Per view rather than per document,
  /// because it holds the document this view last showed, and two views of one
  /// controlled source each fade on their own.
  downstream::RevealTracker _reveal;
  /// The fade's options, resolved from the props and from the platform's own
  /// animation scale.
  downstream::RevealOptions _revealOptions;
  /// Runs only while something is still fading, and in the common run loop modes
  /// so it keeps going while a list scrolls.
  CADisplayLink *_revealDisplayLink;
  /// Whether Reduce Motion was on the last time it was read, so a change is one
  /// comparison rather than an observer per unit.
  BOOL _reduceMotion;
}

+ (ComponentDescriptorProvider)componentDescriptorProvider {
  return concreteComponentDescriptorProvider<DownstreamMarkdownViewComponentDescriptor>();
}

- (instancetype)initWithFrame:(CGRect)frame {
  if (self = [super initWithFrame:frame]) {
    static const auto defaultProps = std::make_shared<const DownstreamMarkdownViewProps>();
    _props = defaultProps;

    // The default theme, so a view that mounts before its props arrive has
    // something to build with rather than an unthemed rendering it would then
    // throw away.
    _theme = downstream::Theme::defaultTheme();
    _themeId = _theme->id();
    _syntaxHighlighting = YES;
    _selectable = YES;
    _controls = defaultProps->controls;
    // The default prop values, so a props update that never mentions one of them
    // compares equal rather than looking like a change.
    _codeLayout.props = {defaultProps->codeBlockWrap, defaultProps->lineNumbers,
                         static_cast<float>(defaultProps->codeBlockMaxHeight)};
    // The prop's own default, so a props update that never mentions the cap
    // compares equal rather than looking like a change.
    _tableLayout.props.maxHeight = static_cast<float>(defaultProps->tableMaxHeight);
    _choices = downstream::ReaderChoices{};
    _codeFilename = "file";
    _tableFilename = "table";
    _imageFilename = "image";
    _csvSeparator = @",";
    _controlLabels = @[];
    _controlIcons = @[];
    _exporter = [DSFileExporter new];
    // One observer per view for the platform's own Reduce Motion setting, which a
    // reader can turn on while a message is still fading.
    [[NSNotificationCenter defaultCenter] addObserver:self
                                             selector:@selector(reduceMotionChanged:)
                                                 name:UIAccessibilityReduceMotionStatusDidChangeNotification
                                               object:nil];
    _documentView = [[DSDocumentView alloc] initWithFrame:self.bounds];
    _documentView.linkDelegate = self;
    _documentView.controlDelegate = self;
    _documentView.imageDelegate = self;
    // The screen scale, so a 4,000-pixel photo costs a phone-sized bitmap rather
    // than a full-size one, and so a decoded picture is the size its box was
    // measured at.
    _documentView.screenScale = UIScreen.mainScreen.scale;
    _documentView.selectable = _selectable;
    _documentView.controls = _controls;
    self.contentView = _documentView;
    // Above the document, never inside it: see `mountChildComponentView:`.
    _hostContainer = [[UIView alloc] initWithFrame:self.bounds];
    _hostContainer.userInteractionEnabled = YES;
    [self addSubview:_hostContainer];

    UITapGestureRecognizer *tap = [[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(handleTap:)];
    tap.cancelsTouchesInView = NO;
    [self addGestureRecognizer:tap];
    // A hold is recognised beside the tap rather than through it, so the tap's
    // timing is unchanged for every press that is not a hold.
    UILongPressGestureRecognizer *hold =
        [[UILongPressGestureRecognizer alloc] initWithTarget:self action:@selector(handleLongPress:)];
    hold.cancelsTouchesInView = NO;
    [self addGestureRecognizer:hold];
  }
  return self;
}

- (void)updateState:(const State::Shared &)state oldState:(const State::Shared &)oldState {
  _state = std::static_pointer_cast<const DownstreamMarkdownShadowNode::ConcreteState>(state);
  if (!_state) {
    return;
  }
  const auto &data = _state->getData();
  // Read before the early-out below: a theme can change on its own, with the
  // document, the scale and the code layout all the same, and then nothing else
  // here rebuilds anything.
  const BOOL themeChanged = [self applyTheme];
  if (!themeChanged && data.document == _document && data.fontSizeMultiplier == _fontScale &&
      data.chrome == _chrome && data.code == _codeLayout.props &&
      data.tables == _tableLayout.props && data.choices == _choices && data.images == _imageState &&
      data.hosts == _hostLayout) {
    return;
  }
  _document = data.document;
  _fontScale = data.fontSizeMultiplier;
  _chrome = data.chrome;
  _codeLayout = downstream::codeLayout(data.code, data.choices);
  _tableLayout = downstream::tableLayout(data.tables, data.choices);
  _choices = data.choices;
  _imageState = data.images;
  _hostLayout = data.hosts;
  _documentView.codeLayout = _codeLayout;
  _documentView.tableLayout = _tableLayout;
  _documentView.imageState = _imageState;
  _documentView.hostLayout = _hostLayout;
  const uint64_t documentId = _document ? _document->id : 0;
  const BOOL tracing = documentId && downstream::SessionTrace::enabledDocument(documentId);
  if (tracing) downstream::SessionTrace::recordDocument(documentId, "apply_start");
  // applyRendering ends the trace the previous document was waiting on.
  [self applyRendering];
  if (tracing) {
    downstream::SessionTrace::recordDocument(documentId, "apply_end");
    _traceDocumentId = documentId;
    [self scheduleTraceFrame];
  }
}

- (void)scheduleTraceFrame {
  if (!_traceDocumentId || _traceDisplayLink || !self.window) return;
  _traceDisplayLink = [CADisplayLink displayLinkWithTarget:self selector:@selector(traceDisplayTick:)];
  [_traceDisplayLink addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
}

#pragma mark - DSRevealProvider

/// The runs of `range` whose alpha is below 1, in the asking layout's own text.
///
/// The provider is asked rather than pushing: a draw pass already knows which
/// characters it is about to draw, so the mapping from a document block's offsets
/// into that layout happens once per drawn range rather than once per frame for
/// the whole document.
- (NSArray<DSRevealRun *> *)revealRunsForCharacterRange:(NSRange)range
                                            blockOffset:(NSInteger)blockOffset
                                            blockRanges:(NSArray<NSValue *> *)blockRanges
                                                     now:(double)now {
  if (blockOffset < 0 || blockRanges.count == 0 || range.length == 0 || _reveal.units().empty()) {
    return @[];
  }
  const double nowMs = now * 1000.0;
  NSMutableArray<DSRevealRun *> *runs = [NSMutableArray new];
  NSRange pending = NSMakeRange(0, 0);
  CGFloat pendingAlpha = 1;
  CGFloat pendingRise = 0;
  const auto flush = [&] {
    if (pending.length > 0) {
      [runs addObject:[[DSRevealRun alloc] initWithRange:pending alpha:pendingAlpha rise:pendingRise]];
    }
  };
  for (const downstream::RevealUnit &unit : unitsForLayout(_reveal.units(), blockOffset, blockRanges.count)) {
    const NSUInteger index = unit.block - static_cast<uint32_t>(blockOffset);
    const NSRange base = blockRanges[index].rangeValue;
    const NSUInteger from = base.location + std::min<NSUInteger>(unit.start, base.length);
    const NSUInteger to = base.location + std::min<NSUInteger>(unit.end, base.length);
    if (from >= NSMaxRange(range)) {
      break;
    }
    if (to <= range.location || from >= to) {
      continue;
    }
    const NSRange clipped = NSIntersectionRange(NSMakeRange(from, to - from), range);
    const float alpha = downstream::revealAlpha(unit, nowMs, _revealOptions);
    if (alpha >= 1) {
      continue;
    }
    // Rise uses the same eased progress as alpha. Do not solve the curve twice.
    const CGFloat rise = _revealOptions.effect == downstream::RevealEffect::Rise
        ? downstream::kRevealRisePoints * (1.0f - alpha) : 0;
    if (pending.length > 0 && NSMaxRange(pending) == clipped.location &&
        pendingAlpha == alpha && pendingRise == rise) {
      // Most units in a backlog are still hidden. One contiguous hidden run
      // carries exactly the same draw/frontier information as thousands.
      pending.length += clipped.length;
    } else {
      flush();
      pending = clipped;
      pendingAlpha = alpha;
      pendingRise = rise;
    }
  }
  flush();
  // Both block ranges and tracker units are already in text order.
  return runs;
}

- (NSUInteger)revealFrontierForCharacterRange:(NSRange)range
                                   blockOffset:(NSInteger)blockOffset
                                   blockRanges:(NSArray<NSValue *> *)blockRanges
                                            now:(double)now {
  if (blockOffset < 0 || range.length == 0) {
    return NSNotFound;
  }
  for (const downstream::RevealUnit &unit : unitsForLayout(_reveal.units(), blockOffset, blockRanges.count)) {
    const NSUInteger index = unit.block - static_cast<uint32_t>(blockOffset);
    const NSRange base = blockRanges[index].rangeValue;
    const NSUInteger from = base.location + std::min<NSUInteger>(unit.start, base.length);
    const NSUInteger to = base.location + std::min<NSUInteger>(unit.end, base.length);
    if (from >= NSMaxRange(range)) {
      break;
    }
    if (to <= range.location || from >= to) {
      continue;
    }
    if (downstream::revealAlpha(unit, now * 1000.0, _revealOptions) <= 0) {
      return std::max(from, range.location);
    }
  }
  return NSNotFound;
}

- (CGFloat)revealAlphaAtBlock:(NSInteger)block offset:(NSUInteger)offset now:(double)now {
  if (block < 0) {
    return 1;
  }
  const auto units = downstream::revealUnitsForBlock(_reveal.units(), static_cast<uint32_t>(block));
  if (units.empty()) {
    return 1;
  }
  const auto after = std::upper_bound(units.begin(), units.end(), offset,
      [](NSUInteger at, const downstream::RevealUnit &unit) { return at < unit.start; });
  if (after == units.begin()) {
    return 1;
  }
  const auto &unit = *std::prev(after);
  return offset < unit.end ? downstream::revealAlpha(unit, now * 1000.0, _revealOptions) : 1;
}

- (void)didMoveToWindow {
  [super didMoveToWindow];
  [self scheduleTraceFrame];
  // A view that leaves its window finishes everything: a fade nobody is looking at
  // is not worth a frame, and finishing it means returning to the window shows the
  // text rather than resuming a stale animation.
  if (self.window == nil) {
    [self finishReveal];
  } else if (_reveal.active()) {
    [self startRevealTicker];
  }
}

- (void)traceDisplayTick:(CADisplayLink *)link {
  [link invalidate];
  if (_traceDisplayLink != link) return;
  _traceDisplayLink = nil;
  if (_document && _document->id == _traceDocumentId && self.window) {
    // A display-link tick follows the mount; it is an estimate, not a GPU paint timestamp.
    downstream::SessionTrace::recordDocument(_traceDocumentId, "frame_estimate");
    _traceDocumentId = 0;
  }
}

/// Adopts the state's theme and reports whether it was a different one.
///
/// It comes from the state rather than from the props because the node interned
/// the theme already: interning again would hash the same slots on the main
/// thread to get a handle the node is holding, and comparing ids is then enough
/// to know whether anything changed at all.
- (BOOL)applyTheme {
  const uint64_t id = _state ? _state->getData().themeId() : 0;
  if (id == _themeId) {
    return NO;
  }
  _themeId = id;
  _theme = _state ? _state->getData().resolvedTheme() : downstream::Theme::defaultTheme();
  _documentView.selectionColor = [DSThemeColors colorsFor:*_theme].selectionColor;
  return YES;
}

/// The strings of a codegen string array, as the view holds them.
- (NSArray<NSString *> *)stringsOf:(const std::vector<std::string> &)values {
  NSMutableArray<NSString *> *out = [NSMutableArray arrayWithCapacity:values.size()];
  for (const std::string &value : values) {
    [out addObject:@(value.c_str())];
  }
  return out;
}

/// The numbers of a codegen number array.
- (NSArray<NSNumber *> *)numbersOf:(const std::vector<double> &)values {
  NSMutableArray<NSNumber *> *out = [NSMutableArray arrayWithCapacity:values.size()];
  for (double value : values) {
    [out addObject:@(value)];
  }
  return out;
}

/// Interns the theme the props carry, or falls back to the default one.
- (void)updateProps:(const Props::Shared &)props oldProps:(const Props::Shared &)oldProps {
  const auto &newProps = *std::static_pointer_cast<const DownstreamMarkdownViewProps>(props);
  // Fabric passes no old props when it mounts a new or recycled view; the
  // view's current props are the previous value then.
  const auto &previous = *std::static_pointer_cast<const DownstreamMarkdownViewProps>(oldProps ? oldProps : _props);
  if (newProps.sessionId != previous.sessionId || newProps.generation != previous.generation) {
    ++_controlEpoch;
    [_documentView resetScrollPositions];
  }
  if (newProps.selectable != _selectable) {
    _selectable = newProps.selectable;
    _documentView.selectable = _selectable;
  }
  if (newProps.controls != _controls) {
    _controls = newProps.controls;
    _documentView.controls = _controls;
  }
  // A chat message that previews every link it holds is a message that
  // navigates by accident, so the default is off and a hold is the app's own
  // `onLinkLongPress`. The preview is a text view behavior, which is why this
  // is a view prop rather than state: it changes no height.
  if (newProps.linkPreview != _linkPreview) {
    _linkPreview = newProps.linkPreview;
    _documentView.linkPreview = _linkPreview;
  }
  // Per-URL link styles, resolved in JavaScript from `markdownStyle.linkVariants`.
  // They change colors but no heights, so they travel as view state rather than
  // through the shadow node: the measurement this view draws from was made
  // without them, and the heights agree either way.
  {
    DSLinkVariants *variants = [DSLinkVariants new];
    variants.prefixes = [self stringsOf:newProps.linkVariantPrefixes];
    variants.counts = [self numbersOf:newProps.linkVariantCounts];
    variants.colors = [self numbersOf:newProps.linkVariantColors];
    variants.underlines = [self numbersOf:newProps.linkVariantUnderline];
    _linkVariants = variants;
    [self applyRendering];
  }
  // The props that decide code layout, gathered into one value. The height and
  // the gutter are heights, so the shadow node measures with them and the view
  // draws with them; a change lands in props a revision before the state that
  // records it does.
  {
    downstream::CodeLayout code = _codeLayout;
    code.props = {newProps.codeBlockWrap, newProps.lineNumbers,
                  static_cast<float>(newProps.codeBlockMaxHeight)};
    if (!(code == _codeLayout)) {
      _codeLayout = code;
      _documentView.codeLayout = code;
    }
  }
  if (newProps.codeFilename != previous.codeFilename) {
    _codeFilename = newProps.codeFilename;
  }
  if (newProps.tableFilename != previous.tableFilename) {
    _tableFilename = newProps.tableFilename;
  }
  if (newProps.imageFilename != previous.imageFilename) {
    _imageFilename = newProps.imageFilename;
  }
  // The image and math props are read by the shadow node, which is what measures;
  // this view reads them only to pick the loader and the image delegate's own
  // policy, and neither changes a height, so nothing is rebuilt here. A change
  // to either reaches the frames through the next state update.
  if (newProps.parseFlags != previous.parseFlags) {
    // A formula that has just become literal, or just become typeset, is a
    // different rendering of the same document, and the shadow node's next
    // layout will have a different height for it.
    [self applyRendering];
  }
  if (newProps.allowedTags != previous.allowedTags) {
    // A tag policy decides which runs exist, so a document parsed under a new one
    // is a different document. The shadow node reads the prop itself and reparses;
    // this only has to redraw once it has.
    [self applyRendering];
  }
  if (newProps.tableMaxHeight != previous.tableMaxHeight) {
    // A height, so it reaches the drawing through the shadow node's state on the
    // next layout, and the shadow node remeasures against it. Nothing is applied
    // here: the state that records it arrives in `applyState`, which is the one
    // path a native view has for changing its own layout without a React render.
  }
  if (newProps.csvSeparator != previous.csvSeparator) {
    _csvSeparator = newProps.csvSeparator.empty() ? @"," : @(newProps.csvSeparator.c_str());
  }
  if (newProps.controlLabels != previous.controlLabels) {
    NSMutableArray<NSString *> *labels = [NSMutableArray arrayWithCapacity:newProps.controlLabels.size()];
    for (const std::string &label : newProps.controlLabels) {
      [labels addObject:@(label.c_str())];
    }
    _controlLabels = labels;
    [_documentView setControlLabels:_controlLabels];
  }
  if (newProps.controlIcons != previous.controlIcons) {
    NSMutableArray<NSString *> *icons = [NSMutableArray arrayWithCapacity:newProps.controlIcons.size()];
    for (const std::string &icon : newProps.controlIcons) {
      [icons addObject:@(icon.c_str())];
    }
    _controlIcons = icons;
    [_documentView setControlIcons:_controlIcons];
  }
  // The chrome itself is part of the state, because it is a height and the
  // shadow node is what measures; a prop change reaches it through a state
  // update on the next layout.
  // Tokens are always computed, so the prop only chooses the rendering. The
  // document's own tokens are unchanged, so nothing needs re-parsing.
  if (newProps.syntaxHighlighting != previous.syntaxHighlighting) {
    _syntaxHighlighting = newProps.syntaxHighlighting;
    // Tokens are computed whatever the flag says, so this only picks the
    // rendering, and the cache keys the two apart.
    [self applyRendering];
  }
  [super updateProps:props oldProps:oldProps];
}

#pragma mark - Hosted children

- (void)layoutSubviews {
  [super layoutSubviews];
  // The container holds nothing of its own, so it takes the document's frame and
  // every child inside it is positioned by Fabric relative to this view, which is
  // the same origin the document draws from.
  _hostContainer.frame = _documentView.frame;
}

/**
 * The document's elements and the hosted children's, in reading order.
 *
 * A container's accessibility elements come after its own elements, so a hosted
 * child appended here would be read after the whole message: VoiceOver would
 * reach the chart a reader can see in the middle of the answer only at the end.
 * This interleaves them by frame instead, which is the order the shadow node
 * placed the children in and therefore the order they are drawn in.
 *
 * The document's own elements carry no frames of their own — they are its
 * virtual text nodes — so a hosted child is spliced in before the first
 * document element that lies below it. That is the same comparison the drawing
 * makes, and it needs no new information from the engine.
 */
- (NSArray<id> *)accessibilityElements {
  NSArray *documentElements = _documentView.accessibilityElements ?: @[];
  NSArray<UIView *> *hosted = _hostContainer.subviews;
  if (hosted.count == 0) {
    return documentElements;
  }
  NSMutableArray<UIView *> *ordered = [hosted mutableCopy];
  [ordered sortUsingComparator:^NSComparisonResult(UIView *a, UIView *b) {
    const CGFloat ay = CGRectGetMinY(a.frame);
    const CGFloat by = CGRectGetMinY(b.frame);
    if (ay == by) {
      return NSOrderedSame;
    }
    return ay < by ? NSOrderedAscending : NSOrderedDescending;
  }];

  NSMutableArray<id> *interleaved = [NSMutableArray arrayWithCapacity:documentElements.count + ordered.count];
  NSUInteger next = 0;
  for (id element in documentElements) {
    // The element's own frame is in the document view's coordinates, which are
    // this view's too, so the comparison is a comparison of like with like.
    const CGRect frame = [element isKindOfClass:UIView.class]
                             ? ((UIView *)element).frame
                             : CGRectZero;
    while (next < ordered.count && frameIsAbove(frame, ordered[next])) {
      [interleaved addObject:ordered[next]];
      next++;
    }
    [interleaved addObject:element];
  }
  while (next < ordered.count) {
    [interleaved addObject:ordered[next]];
    next++;
  }
  return interleaved;
}

/**
 * Mounts a hosted React child, in React's order, above the document view.
 *
 * Neither superclass method is called: `RCTViewComponentView` mounts into
 * `currentContainerView`, which for this view is `_documentView`, and a child
 * inside the document would be laid out in the document's coordinate space,
 * clipped by its bounds, and would hand touches to a text view rather than to
 * itself. The host container is a sibling of the document view instead, so the
 * frame Fabric computes — in this view's coordinates, which are the document
 * view's — is the frame the child draws at.
 *
 * The container exists rather than mounting into `self` directly because
 * `self` also holds the document view, and Fabric addresses children by index:
 * inserting a slot at index 1 would otherwise push the document to index 1 and
 * every subsequent mount and unmount would be one out of step.
 */
- (void)mountChildComponentView:(UIView<RCTComponentViewProtocol> *)childComponentView index:(NSInteger)index {
  RCTAssert(
      childComponentView.superview == nil,
      @"Attempt to mount already mounted component view. (parent: %@, child: %@, index: %@)",
      self,
      childComponentView,
      @(index));
  _hostContainer.frame = _documentView.frame;
  [_hostContainer insertSubview:childComponentView atIndex:index];
}

- (void)unmountChildComponentView:(UIView<RCTComponentViewProtocol> *)childComponentView index:(NSInteger)index {
  RCTAssert(
      (_hostContainer.subviews.count > index) &&
          [_hostContainer.subviews objectAtIndex:index] == childComponentView,
      @"Attempt to unmount a view which has a different index. (parent: %@, child: %@, index: %@)",
      self,
      childComponentView,
      @(index));
  [childComponentView removeFromSuperview];
}

- (void)prepareForRecycle {
  ++_controlEpoch;
  if (_traceDocumentId) downstream::SessionTrace::recordDocument(_traceDocumentId, "frame_unavailable");
  _traceDocumentId = 0;
  [_traceDisplayLink invalidate];
  _traceDisplayLink = nil;
  // A recycled view is about to show a different message, so it has to forget
  // what it had shown. `clear` also finishes, which is what stops the ticker and
  // leaves the next document to fade from its first word rather than to resume
  // this one's timing.
  [self stopReveal];
  _reveal.clear();
  [super prepareForRecycle];
  _state.reset();
  _document.reset();
  _documentView.rendered = nil;
}

- (void)dealloc {
  [[NSNotificationCenter defaultCenter] removeObserver:self];
  if (_traceDocumentId) downstream::SessionTrace::recordDocument(_traceDocumentId, "frame_unavailable");
  [_traceDisplayLink invalidate];
  [_revealDisplayLink invalidate];
}

/// Rebuilds the platform text from the document the view already holds, and
/// ends any frame trace the old rendering was waiting on.
///
/// A color appearance change deliberately does not come through here. Token
/// colors are dynamic UIColors, which the platform resolves against the trait
/// collection when it draws, so a cached rendering already follows the
/// appearance and rebuilding it would only discard warm caches to get the same
/// objects back.
- (void)applyRendering {
  // The caret the state resolved. It is in the rendering's cache key, so a change
  // rebuilds only the last segment rather than the document.
  const downstream::PlacedCaret caret = _state ? _state->getData().caret : downstream::PlacedCaret{};
  const uint64_t documentId = _document ? _document->id : 0;
  if (_traceDocumentId && _traceDocumentId != documentId) {
    downstream::SessionTrace::recordDocument(_traceDocumentId, "frame_unavailable");
    [_traceDisplayLink invalidate];
    _traceDisplayLink = nil;
    _traceDocumentId = 0;
  }
  // The fade's ticker is separate from the trace's: one runs for as long as
  // something is fading and the other for a single frame, so neither invalidates
  // the other.
  _documentView.chrome = _chrome;
  _documentView.rendered = _document ? [DSRenderedDocument renderedDocumentFor:_document
                                                                  fontScale:_fontScale
                                                         syntaxHighlighting:_syntaxHighlighting
                                                                      theme:_theme
                                                                     caret:caret
                                                                  variants:_linkVariants]
                                     : nil;
  // The fade is decided after the rendering exists, because the tracker aligns the
  // document it was given against the one this view last showed, and the ticker
  // needs the segment views the rendering just produced.
  [self applyReveal];
}

#pragma mark - The fade

/// Whether the system asks for reduced motion.
///
/// A reader's setting rather than an app's, so it is read here rather than
/// resolved in JavaScript: JavaScript cannot see it, and a fade that ignored it
/// would be a fade some readers cannot turn off. The notification is one
/// comparison per change rather than an observer per unit.
- (BOOL)reduceMotionEnabled {
  return UIAccessibilityIsReduceMotionEnabled();
}

/// The fade's options, from the props.
///
/// The three time values are the engine's own, already resolved by
/// `src/animation.ts`, so there is one place that decides what `stagger` means. A
/// curve's x components are clamped again here because the boundary is what trusts
/// them: a curve that doubles back has no single y for an x, which is the one
/// thing a fade needs.
- (downstream::RevealOptions)resolveRevealOptions:(const DownstreamMarkdownViewProps &)props {
  downstream::RevealOptions options;
  // `animation` and `animationSplit` arrive as codegen enums, so the string
  // names were already parsed once, in `fromRawValue`.
  switch (props.animation) {
    case DownstreamMarkdownViewAnimation::FadeIn:
      options.effect = downstream::RevealEffect::Fade;
      break;
    case DownstreamMarkdownViewAnimation::SlideUp:
      options.effect = downstream::RevealEffect::Rise;
      break;
    case DownstreamMarkdownViewAnimation::None:
      break;
  }
  options.split = props.animationSplit == DownstreamMarkdownViewAnimationSplit::Char
                      ? downstream::RevealSplit::Char
                      : downstream::RevealSplit::Word;
  if (props.animationTiming.size() >= 7) {
    options.durationMs = static_cast<float>(props.animationTiming[0]);
    options.staggerMs = static_cast<float>(props.animationTiming[1]);
    options.maxBacklogMs = static_cast<float>(props.animationTiming[2]);
    const auto clamp = [](double value) { return static_cast<float>(std::min(1.0, std::max(0.0, value))); };
    options.easing = {clamp(props.animationTiming[3]), static_cast<float>(props.animationTiming[4]),
                      clamp(props.animationTiming[5]), static_cast<float>(props.animationTiming[6])};
  }
  // Reduce Motion turns the fade off rather than shortening it: the setting asks
  // for no animation, not for a faster one. The caret is unaffected, because it
  // does not move.
  if ([self reduceMotionEnabled]) {
    options.effect = downstream::RevealEffect::None;
  }
  return options;
}

/// Aligns the document this view was just given against the one it last showed,
/// schedules what is new, and starts the ticker if anything is still fading.
- (void)applyReveal {
  _revealOptions = [self resolveRevealOptions:*std::static_pointer_cast<const DownstreamMarkdownViewProps>(_props)];
  _documentView.revealProvider =
      _revealOptions.effect == downstream::RevealEffect::None ? nil : (id<DSRevealProvider>)self;
  if (!_document) {
    [self stopReveal];
    return;
  }
  const double now = CACurrentMediaTime() * 1000.0;
  if (_reveal.apply(_document, now, _revealOptions).empty()) {
    [self stopReveal];
    return;
  }
  [self startRevealTicker];
}

/// The display link, in the common run loop modes so a fade keeps running while
/// the reader scrolls a list. One per view, separate from the trace's link: one
/// runs for as long as something is fading and the other for a single frame.
- (void)startRevealTicker {
  if (_revealDisplayLink != nil || !self.window) {
    return;
  }
  _revealDisplayLink = [CADisplayLink displayLinkWithTarget:self selector:@selector(revealTick:)];
  [_revealDisplayLink addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
}

- (void)revealTick:(CADisplayLink *)link {
  if (_revealDisplayLink != link) {
    return;
  }
  [self invalidateReveals];
  // `prune` drops what has finished, and the frame after that is the one that
  // draws it at full alpha, so it is dropped here rather than the moment its fade
  // ended. What it drops still redraws at full alpha this frame.
  if (!_reveal.prune(CACurrentMediaTime() * 1000.0, _revealOptions)) {
    [self stopReveal];
  }
}

/// Redraws only the lines that hold an active unit.
///
/// `invalidateDisplayForCharacterRange:` is the documented trigger and TextKit
/// redraws only the line fragments that range touches. No text storage is edited,
/// no glyph is invalidated, and nothing is laid out, which is what keeps a frame
/// of animation free of every cost the rest of this library avoids.
- (void)invalidateReveals {
  if (!_state) {
    return;
  }
  const auto &document = _state->getData().document;
  if (!document) {
    return;
  }
  [_documentView invalidateRevealUnits:_reveal.units()
                             document:*document
                                  now:CACurrentMediaTime() * 1000.0];
}

/// Stops the ticker without finishing anything.
- (void)stopReveal {
  [_revealDisplayLink invalidate];
  _revealDisplayLink = nil;
}

/// Finishes every fade at once and redraws.
///
/// Three callers need the same thing: Reduce Motion, an explicit `animated: false`
/// mid-fade, and a view leaving its window. All three want the text at once
/// rather than a fade nobody is watching finish.
- (void)finishReveal {
  const bool hadUnits = _reveal.active();
  // A parent view redraw does not dirty TextKit child views. Invalidate the
  // affected ranges while they are still available, then draw them at full alpha.
  if (hadUnits) {
    [self invalidateReveals];
  }
  _reveal.clear();
  [self stopReveal];
  if (hadUnits) {
    [_documentView setNeedsDisplay];
  }
}

- (void)reduceMotionChanged:(NSNotification *)note {
  (void)note;
  // Every unit finishes at once and later snapshots schedule nothing, which is
  // what the setting asks for.
  [self finishReveal];
  [self applyReveal];
}

#pragma mark - Links

- (void)handleTap:(UITapGestureRecognizer *)recognizer {
  if (recognizer.state != UIGestureRecognizerStateEnded) {
    return;
  }
  const NSInteger linkIndex = [_documentView linkIndexAtPoint:[recognizer locationInView:_documentView]];
  if (linkIndex >= 0) {
    [self didPressLinkAtIndex:linkIndex];
  }
}

/// A hold on a link, for the app to do something else with.
///
/// Installed beside the tap rather than instead of it, because the two gestures
/// recognise independently: a hold that becomes a tap still taps, and a tap never
/// waits to find out whether it was a hold.
- (void)handleLongPress:(UILongPressGestureRecognizer *)recognizer {
  if (recognizer.state != UIGestureRecognizerStateBegan) {
    return;
  }
  const NSInteger linkIndex = [_documentView linkIndexAtPoint:[recognizer locationInView:_documentView]];
  if (linkIndex >= 0) {
    [self didLongPressLinkAtIndex:linkIndex];
  }
}

- (void)didPressLinkAtIndex:(NSInteger)linkIndex {
  if (!_state || !_eventEmitter) {
    return;
  }
  const auto &document = _state->getData().document;
  if (!document || linkIndex < 0 || static_cast<size_t>(linkIndex) >= document->links.size()) {
    return;
  }
  const auto &link = document->links[static_cast<size_t>(linkIndex)];
  std::static_pointer_cast<const DownstreamMarkdownViewEventEmitter>(_eventEmitter)
      ->onLinkPress({
          .url = link.url,
          .start = static_cast<int>(link.range.start),
          .end = static_cast<int>(link.range.end),
      });
}

/// A long press on a link, reported rather than acted on.
///
/// A tap opens (or asks, through link safety); a hold is the app's: copy the URL,
/// show a preview, open a sheet. The library does nothing with it, because doing
/// anything would be a second behavior for one gesture.
- (void)didLongPressLinkAtIndex:(NSInteger)linkIndex {
  if (!_state || !_eventEmitter) {
    return;
  }
  const auto &document = _state->getData().document;
  if (!document || linkIndex < 0 || static_cast<size_t>(linkIndex) >= document->links.size()) {
    return;
  }
  const auto &link = document->links[static_cast<size_t>(linkIndex)];
  std::static_pointer_cast<const DownstreamMarkdownViewEventEmitter>(_eventEmitter)
      ->onLinkLongPress({
          .url = link.url,
          .start = static_cast<int>(link.range.start),
          .end = static_cast<int>(link.range.end),
      });
}

#pragma mark - Controls

/// The event emitter, or null when the view has been recycled. A control that
/// outlives its view must report nothing: there is no JavaScript left to
/// report it to.
- (const DownstreamMarkdownViewEventEmitter *)emitter {
  return _eventEmitter
      ? std::static_pointer_cast<const DownstreamMarkdownViewEventEmitter>(_eventEmitter).get()
      : nullptr;
}

/// A press on a loaded image that is not inside a link.
///
/// The library opens nothing. An app that has a viewer wants its own, and one
/// that does not should not have a picture that navigates somewhere surprising
/// when tapped, so the press is reported and the decision is the app's.
- (void)didPressImageWithURL:(NSString *)url
                         alt:(NSString *)alt
                       title:(NSString *)title
                    ordinal:(uint32_t)ordinal {
  const auto *emitter = [self emitter];
  if (emitter == nullptr || !_document || _document->images.size() <= ordinal) {
    return;
  }
  // The image's own source range, so a press maps back to the `![…](…)` the
  // writer sent rather than to a position in a rendered view.
  const auto &image = _document->images[ordinal];
  emitter->onImagePress({
      .url = std::string(url.UTF8String ?: ""),
      .alt = std::string(alt.UTF8String ?: ""),
      .title = std::string(title.UTF8String ?: ""),
      .start = static_cast<int>(image.range.start),
      .end = static_cast<int>(image.range.end),
  });
}

- (void)emitControlWithBlock:(NSString *)block
                      action:(NSString *)action
                      format:(NSString *)format
                    language:(NSString *)language
                    filename:(NSString *)filename
                        url:(NSString *)url
                     wrapped:(BOOL)wrapped
                    expanded:(BOOL)expanded
                    markdown:(NSString *)markdown {
  const auto *emitter = [self emitter];
  if (emitter == nullptr) {
    return;
  }
  // The press carries the generation, so a document that has been replaced
  // since drops it rather than reporting it to the new one's handlers.
  const int generation = _state
      ? static_cast<int>(std::static_pointer_cast<const DownstreamMarkdownViewProps>(_props)->generation)
      : 0;
  emitter->onControlAction({
      .block = std::string(block.UTF8String),
      .action = std::string(action.UTF8String),
      .format = std::string(format.UTF8String),
      .language = std::string(language.UTF8String),
      .filename = std::string(filename.UTF8String),
      // Only an image carries a destination: a code block and a table name a
      // file rather than a place to fetch from, and an empty string here is what
      // JavaScript reads as "no url" rather than as a failed request.
      .url = std::string(url.UTF8String ?: ""),
      .wrapped = static_cast<bool>(wrapped),
      .expanded = static_cast<bool>(expanded),
      // Only a full-screen table carries Markdown: the modal it opens holds a
      // second view, and the text it renders is the table's own source rather
      // than a rebuild of it. Empty for every other action.
      .markdown = std::string(markdown.UTF8String ?: ""),
      // Every press reports its block's ordinal among its own kind, because an
      // app that keys a press needs to know which block was pressed and three
      // kinds count separately. -1 where there is no ordinal to report.
      .ordinal = -1,
      .generation = generation,
  });
}

- (void)emitControlFailureFor:(NSString *)block action:(NSString *)action message:(NSString *)message {
  if (!_state || _eventEmitter == nullptr) {
    return;
  }
  // The block and action travel as fields, so routing a failure to its own
  // onError does not depend on how the message reads and an app is told which
  // control failed. The message still names it, for a log line.
  std::static_pointer_cast<const DownstreamMarkdownViewEventEmitter>(_eventEmitter)
      ->onMarkdownError({
          .code = "control_failed",
          .message = std::string([NSString stringWithFormat:@"%@ %@ failed: %@", block, action, message]
                                      .UTF8String),
          .generation = static_cast<int>(std::static_pointer_cast<const DownstreamMarkdownViewProps>(_props)->generation),
          .sequence = 0,
          .recoverable = true,
          .block = std::string(block.UTF8String),
          .action = std::string(action.UTF8String),
      });
}

- (void)didPressControlOfKind:(DSBlockKind)kind
                   blockIndex:(NSInteger)blockIndex
                   blockCount:(uint32_t)blockCount
                     language:(NSString *)language
                      action:(NSString *)action
                      format:(NSString *)format
                    filename:(NSString *)filename {
  if (kind == DSBlockKindImage) {
    // An image has no block range to read: the segment view fetched the bytes,
    // named the file, and presented the dialog, so all that is left is to report
    // what happened. The URL is what JavaScript needs to open its own viewer, and
    // the file name is what it would have named the file itself.
    if (filename.length == 0 || ![action isEqualToString:@"download"]) {
      [self emitControlWithBlock:@"image"
                          action:action
                          format:@""
                        language:@""
                        filename:@""
                            url:@""
                         wrapped:NO
                        expanded:NO
                        markdown:@""];
      return;
    }
    [self emitControlWithBlock:@"image"
                        action:@"download"
                        format:@""
                      language:@""
                      filename:filename
                          url:_pressedImageURL ?: @""
                       wrapped:NO
                      expanded:NO
                        markdown:@""];
    return;
  }
  if (!_document) {
    return;
  }
  const bool isCode = kind == DSBlockKindCode;
  // The kind says which block type the range holds and the index says where it
  // starts, so the range is read from the index and the kind only chooses the
  // format. A range whose first block is not of that kind is a caller bug, and
  // reporting it beats copying whatever happens to live at those indexes.
  if (blockIndex < 0 || static_cast<size_t>(blockIndex) >= _document->blocks.size()) {
    [self emitControlFailureFor:isCode ? @"code" : @"table" action:action message:@"The block is gone."];
    return;
  }
  const size_t from = static_cast<size_t>(blockIndex);
  const size_t to = std::min(from + blockCount, _document->blocks.size());
  if (from >= to) {
    return;
  }
  if ((_document->blocks[from].kind == BlockKind::CodeBlock) != isCode) {
    [self emitControlFailureFor:isCode ? @"code" : @"table"
                         action:action
                        message:@"The control and the block it names disagree."];
    return;
  }
  const bool download = [action isEqualToString:@"download"];
  const uint64_t copyEpoch = download ? 0 : ++copyRequestEpoch;
  const NSInteger clipboardChangeCount = download ? 0 : UIPasteboard.generalPasteboard.changeCount;
  const char16_t separator = download ? resolveSeparator(_csvSeparator) : kDefaultCsvSeparator;
  // Code always copies the text it shows; a table is copied in the format
  // its menu chose.
  const ExportFormat exportFormat = isCode ? ExportFormat::Text : static_cast<ExportFormat>(formatFor(format));

  // Retain the immutable snapshot, not the view. Streaming can publish newer
  // snapshots while this worker serializes the settled range that was pressed.
  const DocumentHandle document = _document;
  const uint64_t epoch = _controlEpoch;
  const std::string codeFilename = _codeFilename;
  const std::string tableFilename = _tableFilename;
  __weak DownstreamMarkdownView *weakSelf = self;
  void (^finish)(NSString *, NSString *, NSData *, NSString *, NSString *) =
      ^(NSString *text, NSString *html, NSData *data, NSString *name, NSString *failure) {
    DownstreamMarkdownView *view = weakSelf;
    if (view == nil || view->_controlEpoch != epoch || !view->_state || !view->_eventEmitter ||
        (!download && (copyEpoch != copyRequestEpoch ||
                       UIPasteboard.generalPasteboard.changeCount != clipboardChangeCount))) {
      return;
    }
    if (failure != nil) {
      [view emitControlFailureFor:isCode ? @"code" : @"table" action:action message:failure];
      return;
    }
    if (!download) {
      if (isCode) {
        UIPasteboard.generalPasteboard.string = text;
      } else {
        [UIPasteboard.generalPasteboard setItems:@[
          @{@"public.utf8-plain-text" : text, @"public.html" : html},
        ]];
      }
      [view emitControlWithBlock:isCode ? @"code" : @"table"
                          action:action
                          format:format
                        language:language
                        filename:@""
                             url:@""
                         wrapped:NO
                        expanded:NO
                        markdown:@""];
      return;
    }
    [view->_exporter saveData:data
                    fileName:name
               shouldPresent:^BOOL {
      DownstreamMarkdownView *current = weakSelf;
      return current != nil && current->_controlEpoch == epoch && current->_state && current->_eventEmitter;
    }
                   completion:^(NSURL *url, NSError *error) {
      DownstreamMarkdownView *current = weakSelf;
      if (current == nil || current->_controlEpoch != epoch || !current->_state || !current->_eventEmitter) {
        return;
      }
      if (error != nil) {
        [current emitControlFailureFor:isCode ? @"code" : @"table"
                                action:@"download"
                               message:error.localizedDescription];
      } else if (url != nil) {
        [current emitControlWithBlock:isCode ? @"code" : @"table"
                               action:action
                               format:format
                             language:language
                             filename:name
                                 url:@""
                              wrapped:NO
                             expanded:NO
                        markdown:@""];
      }
    }];
  };

  size_t estimatedBytes = 0;
  for (size_t index = from; index < to && estimatedBytes <= kInlineExportBytes; ++index) {
    const Block &block = document->blocks[index];
    if (block.text.size() > kInlineExportBytes / sizeof(char16_t) ||
        block.cells.size() > kInlineExportBytes / 32) {
      estimatedBytes = kInlineExportBytes + 1;
      break;
    }
    estimatedBytes += block.text.size() * sizeof(char16_t) + block.cells.size() * 32 + 64;
  }
  const BOOL inlineExport = estimatedBytes <= kInlineExportBytes;
  void (^serialize)(void) = ^{
    @autoreleasepool {
      NSString *text = nil;
      NSString *html = nil;
      NSData *data = nil;
      NSString *name = @"";
      NSString *failure = nil;
      try {
        text = toNSString(exportBlocks(*document, from, to, exportFormat, separator));
        if (download) {
          name = isCode ? toNSString(downloadName(document->blocks[from].meta, document->blocks[from].language,
                                                 codeFilename, "file"))
                        : toNSString(tableDownloadName(tableFilename, std::string_view(nameForFormat(format).UTF8String)));
          NSMutableData *bytes = [NSMutableData data];
          // Excel on Windows needs the BOM to identify UTF-8 CSV text.
          if ([format isEqualToString:@"csv"]) {
            const uint8_t bom[] = {0xEF, 0xBB, 0xBF};
            [bytes appendBytes:bom length:sizeof(bom)];
          }
          [bytes appendData:[text dataUsingEncoding:NSUTF8StringEncoding]];
          data = [bytes copy];
        } else if (!isCode) {
          html = toNSString(exportBlocks(*document, from, to, ExportFormat::Html, separator));
        }
      } catch (const std::exception &error) {
        failure = toNSString(std::string(error.what()));
      } catch (...) {
        failure = @"Could not serialize the block.";
      }
      if (inlineExport) {
        finish(text, html, data, name, failure);
      } else {
        dispatch_async(dispatch_get_main_queue(), ^{
          finish(text, html, data, name, failure);
        });
      }
    }
  };
  if (inlineExport) {
    serialize();
  } else {
    WorkerPool::shared().post([serialize] { serialize(); });
  }
}

- (void)imagesDidChange {
  if (!_state) {
    return;
  }
  // A clone with the same data is enough: Fabric measures every new revision,
  // and the node's `layout` reads each picture's answer from the cache the load
  // just wrote and commits the state the view then draws.
  _state->updateState(DownstreamMarkdownState(_state->getData()));
}

- (void)didToggleWrapAtOrdinal:(uint32_t)ordinal language:(NSString *)language wrapped:(BOOL)wrapped {
  const auto *emitter = [self emitter];
  if (emitter == nullptr || !_state) {
    return;
  }
  // The height changes, and only Yoga and the chat list around the message can
  // know that, so the change goes back through a Fabric state update rather
  // than a React render: no parse, no JavaScript work, no re-measure of the
  // text, which is laid out again only for the block that changed. The state
  // carries the whole code layout, which is what the toggled set completes, so
  // measurement resolves the same way afterwards.
  DownstreamMarkdownState next = _state->getData().toggledWrap(ordinal, _controls);
  _state->updateState(std::move(next));
  [self emitControlWithBlock:@"code"
                      action:@"wrap"
                      format:@""
                    language:language
                    filename:@""
                        url:@""
                     wrapped:wrapped
                    expanded:NO
                        markdown:@""];
}

- (void)didToggleExpandAtOrdinal:(uint32_t)ordinal language:(NSString *)language expanded:(BOOL)expanded {
  const auto *emitter = [self emitter];
  if (emitter == nullptr || !_state) {
    return;
  }
  // The same path as wrap, and for the same reason: the card's height changes,
  // and only Yoga and the chat list can know that. No React render, no parse, and
  // the button has already acted by the time the event is reported.
  DownstreamMarkdownState next = _state->getData().toggledExpand(ordinal, _controls);
  _state->updateState(std::move(next));
  [self emitControlWithBlock:@"code"
                      action:@"expand"
                      format:@""
                    language:language
                    filename:@""
                        url:@""
                     wrapped:NO
                    expanded:expanded
                        markdown:@""];
}

- (void)didToggleTableExpandAtOrdinal:(uint32_t)ordinal expanded:(BOOL)expanded {
  if (!_state) {
    return;
  }
  // The same path as a code block's expand, and for the same reason: the table's
  // height changes, and only Yoga and the chat list can know that.
  DownstreamMarkdownState next = _state->getData().toggledTableExpand(ordinal);
  _state->updateState(std::move(next));
  [self emitControlWithBlock:@"table"
                      action:@"expand"
                      format:@""
                    language:@""
                    filename:@""
                        url:@""
                     wrapped:NO
                    expanded:expanded
                    markdown:@""];
}

- (void)didToggleDetailsAtContainer:(uint32_t)containerId open:(BOOL)open {
  if (!_state) {
    return;
  }
  // The same path as a code block's expand, and for the same reason: the body's
  // height changes, and only Yoga and the chat list can know that.
  DownstreamMarkdownState next = _state->getData().toggledDetails(containerId);
  _state->updateState(std::move(next));
  [self emitControlWithBlock:@"details"
                      action:@"expand"
                      format:@""
                    language:@""
                   filename:@""
                        url:@""
                     wrapped:NO
                    expanded:open
                   markdown:@""];
}

- (void)didToggleTaskAtContainer:(uint32_t)containerId checked:(BOOL)checked {
  const auto *emitter = [self emitter];
  if (emitter == nullptr) {
    return;
  }
  // A task checkbox is drawn, not stored: the document does not change when a
  // reader taps one, so the press only reports. The app owns the source, which
  // is the only thing that can flip `- [ ]` to `- [x]`.
  const int generation = _state
      ? static_cast<int>(std::static_pointer_cast<const DownstreamMarkdownViewProps>(_props)->generation)
      : 0;
  emitter->onTaskPress({
      .containerId = static_cast<int32_t>(containerId),
      .checked = static_cast<bool>(checked),
      .generation = generation,
  });
}

- (void)didRequestTableFullscreenAtOrdinal:(uint32_t)ordinal {
  const auto *emitter = [self emitter];
  if (emitter == nullptr) {
    return;
  }
  // Nothing is drawn here and nothing is torn down: the button opens a modal in
  // JavaScript, holding a second view, and the table behind it keeps its own
  // cap and its own scroll position. So the press only reports — the table's
  // source, which is what the modal renders.
  const int generation = _state
      ? static_cast<int>(std::static_pointer_cast<const DownstreamMarkdownViewProps>(_props)->generation)
      : 0;
  emitter->onControlAction({
      .block = std::string("table"),
      .action = std::string("fullscreen"),
      .format = std::string(),
      .language = std::string(),
      .filename = std::string(),
      .url = std::string(),
      .wrapped = false,
      .expanded = false,
      .markdown = std::string([self markdownForTableAtOrdinal:ordinal].UTF8String ?: ""),
      .ordinal = static_cast<int32_t>(ordinal),
      .generation = generation,
  });
}

/// The table at `ordinal`'s own Markdown, as the export writes it.
///
/// The same `exportBlocks` a copy produces, so a full-screen table is the table
/// rather than one rebuilt from the model — and a table whose columns were
/// reordered comes back as the pipe table a reader would have written.
- (NSString *)markdownForTableAtOrdinal:(uint32_t)ordinal {
  const auto &document = _state ? _state->getData().document : nullptr;
  if (document == nullptr) {
    return @"";
  }
  const std::vector<SegmentPlan> plan = planSegments(*document);
  uint32_t seen = 0;
  for (const SegmentPlan &segment : plan) {
    if (segment.kind != SegmentKind::Table) {
      continue;
    }
    if (seen++ == ordinal) {
      return toNSString(exportBlocks(*document, segment.from, segment.to, ExportFormat::Markdown));
    }
  }
  return @"";
}

- (void)didFailControlOfKind:(DSBlockKind)kind action:(NSString *)action message:(NSString *)message {
  [self emitControlFailureFor:kind == DSBlockKindCode    ? @"code"
                       : kind == DSBlockKindImage    ? @"image"
                                                      : @"table"
                        action:action
                       message:message];
}


@end

Class<RCTComponentViewProtocol> DownstreamMarkdownViewCls(void) {
  return DownstreamMarkdownView.class;
}
