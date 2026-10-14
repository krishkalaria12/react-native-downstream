#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#import "DSRenderedDocument.h"
#import "DSSegmentViews.h"
#import <objc/runtime.h>
#include "Caret.h"
#include "Document.h"
#include "HostLayout.h"
#include "theme/Theme.h"
#include <chrono>
#include <cmath>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

static void require(BOOL condition, NSString *message) {
  if (!condition) { fprintf(stderr, "FAIL: %s\n", message.UTF8String); exit(1); }
}

static NSUInteger gutterNumberCalls = 0;
static IMP originalNumberForLine;
static NSString *countedNumberForLine(id gutter, SEL selector, uint32_t line) {
  ++gutterNumberCalls;
  return reinterpret_cast<NSString *(*)(id, SEL, uint32_t)>(originalNumberForLine)(gutter, selector, line);
}

static std::atomic<NSUInteger> layoutCalls{0};
static IMP originalEnsureLayout;
static void countedEnsureLayout(id manager, SEL selector, NSTextContainer *container) {
  layoutCalls++;
  reinterpret_cast<void (*)(id, SEL, NSTextContainer *)>(originalEnsureLayout)(manager, selector, container);
}

static NSString *countedText;
static NSUInteger characterReads = 0;
static IMP originalCharacterAtIndex;
static unichar countedCharacterAtIndex(id text, SEL selector, NSUInteger index) {
  if (text == countedText) characterReads++;
  return reinterpret_cast<unichar (*)(id, SEL, NSUInteger)>(originalCharacterAtIndex)(text, selector, index);
}

static UIView *findGutter(UIView *view) {
  Class gutterClass = NSClassFromString(@"DSCodeGutterView");
  if ([view isKindOfClass:gutterClass]) return view;
  for (UIView *child in view.subviews) {
    UIView *found = findGutter(child);
    if (found != nil) return found;
  }
  return nil;
}

static downstream::DocumentHandle makeDocument(NSString *source) {
  static uint64_t nextId = 9001;
  auto document = std::make_shared<downstream::Document>();
  document->id = nextId++;
  downstream::Block block;
  block.kind = downstream::BlockKind::CodeBlock;
  block.language = "text";
  std::vector<unichar> characters(source.length);
  [source getCharacters:characters.data() range:NSMakeRange(0, source.length)];
  block.text = std::u16string(reinterpret_cast<const char16_t *>(characters.data()), characters.size());
  document->blocks.push_back(std::move(block));
  return document;
}

static void checkEmojiGlyphMapping() {
  NSMutableString *source = [NSMutableString string];
  for (NSUInteger i = 0; i < 10; ++i) [source appendString:@"👨‍👩‍👧‍👦"];
  [source appendString:@"\nx"];
  DSRenderedDocument *rendered = [DSRenderedDocument renderedDocumentFor:makeDocument(source)
                                                                fontScale:1
                                                       syntaxHighlighting:NO
                                                                    theme:downstream::Theme::defaultTheme()
                                                              caret:downstream::PlacedCaret{}];
  DSCodeSegmentView *view = [DSCodeSegmentView new];
  DSCodeSegment *emojiSegment = (DSCodeSegment *)rendered.segments.firstObject;
  [view setSegment:emojiSegment];
  downstream::CodeBlockLayout drawLayout{false, true, 1, 0, false};
  view.layout = drawLayout;
  // A complex emoji sequence spans several UTF-16 units. Verify the final
  // logical line maps to a glyph and is still visited by the real gutter.
  DSTextSegmentView *body = [DSTextSegmentView new];
  [body setSegment:emojiSegment.text];
  const NSUInteger characterOffset = source.length - 1;
  const NSUInteger glyph = [body.layoutManager glyphRangeForCharacterRange:NSMakeRange(characterOffset, 0)
                                                     actualCharacterRange:NULL].location;
  printf("emoji_mapping character_offset=%lu glyph_count=%lu mapped_glyph=%lu\n",
         (unsigned long)characterOffset, (unsigned long)body.layoutManager.numberOfGlyphs,
         (unsigned long)glyph);
  require(glyph < body.layoutManager.numberOfGlyphs,
          @"emoji UTF-16 offset did not map to a valid glyph");

  UIView *gutter = findGutter(view);
  require(gutter != nil, @"code view did not create a gutter");
  Method numberMethod = class_getInstanceMethod(object_getClass(gutter), @selector(numberForLine:));
  if (numberMethod == nil) numberMethod = class_getInstanceMethod([gutter class], @selector(numberForLine:));
  require(numberMethod != nil, @"gutter number method was not found");
  originalNumberForLine = method_setImplementation(numberMethod, (IMP)countedNumberForLine);
  gutterNumberCalls = 0;
  [view setFrame:CGRectMake(0, 0, 480, 300)];
  [view layoutIfNeeded];
  UIGraphicsBeginImageContextWithOptions(gutter.bounds.size, NO, 1);
  [gutter drawRect:gutter.bounds];
  UIGraphicsEndImageContext();
  method_setImplementation(numberMethod, originalNumberForLine);
  printf("gutter_number_calls=%lu\n", (unsigned long)gutterNumberCalls);
  require(gutterNumberCalls >= 2, @"gutter draw omitted the final emoji-followed line");
#ifndef BASELINE
  drawLayout.expanded = true;
  view.layout = drawLayout;
  require(view.layout.expanded, @"expanded-only layout change was dropped");
#endif
}

static void checkBlankSuffixDraw() {
  DSRenderedDocument *rendered = [DSRenderedDocument renderedDocumentFor:makeDocument([@"\n" stringByPaddingToLength:4096
                                                                                                               withString:@"\n" startingAtIndex:0])
                                                                fontScale:1 syntaxHighlighting:NO theme:downstream::Theme::defaultTheme()
                                                              caret:downstream::PlacedCaret{}];
  DSCodeSegmentView *view = [DSCodeSegmentView new];
  [view setSegment:rendered.segments.firstObject];
  view.layout = downstream::CodeBlockLayout{false, true, 1, 0, false};
  view.frame = CGRectMake(0, 0, 480, 300);
  [view layoutIfNeeded];
  UIView *gutter = findGutter(view);
  DSTextSegmentView *body = [gutter valueForKey:@"body"];
  countedText = body.textStorage.string;
  Method characterMethod = class_getInstanceMethod([countedText class], @selector(characterAtIndex:));
  originalCharacterAtIndex = method_setImplementation(characterMethod, (IMP)countedCharacterAtIndex);
  characterReads = 0;
  const CGRect dirty = CGRectMake(0, 0, gutter.bounds.size.width, 200);
  UIGraphicsBeginImageContextWithOptions(dirty.size, NO, 1);
  [gutter drawRect:dirty];
  UIGraphicsEndImageContext();
  method_setImplementation(characterMethod, originalCharacterAtIndex);
  countedText = nil;
  printf("blank_suffix_draw_character_reads=%lu\n", (unsigned long)characterReads);
#ifdef BASELINE
  require(characterReads > 4096, @"baseline did not reproduce repeated blank-suffix scans");
#else
  require(characterReads < 100, @"blank-line draw searched outside the visible lines");
#endif
}

#ifndef BASELINE
static void checkGutterOwnerLifetime() {
  UIView *gutter;
  __weak DSCodeSegment *owner;
  @autoreleasepool {
    DSRenderedDocument *rendered = [DSRenderedDocument renderedDocumentFor:makeDocument(@"keep\n\nalive")
                                                                  fontScale:1 syntaxHighlighting:NO theme:downstream::Theme::defaultTheme()
                                                              caret:downstream::PlacedCaret{}];
    DSCodeSegmentView *view = [DSCodeSegmentView new];
    [view setSegment:rendered.segments.firstObject];
    gutter = findGutter(view);
    owner = rendered.segments.firstObject;
    [DSRenderedDocument invalidateCaches];
  }
  require(owner != nil && [gutter valueForKey:@"segment"] == owner, @"gutter did not retain the line-index owner");
  require([owner baselineCharacterOffsetAtOrAfter:5] == 6, @"retained line index is invalid");
}
#endif

static void checkRenderingCacheBudget() {
  [DSRenderedDocument invalidateCaches];
  const auto obtain = [](const downstream::DocumentHandle &document) {
    return [DSRenderedDocument renderedDocumentFor:document fontScale:1 syntaxHighlighting:NO
                                             theme:downstream::Theme::defaultTheme()
                                             caret:downstream::PlacedCaret{}];
  };
  const auto smallDocument = makeDocument(@"short message");
  DSRenderedDocument *small = obtain(smallDocument);
  require(obtain(smallDocument) == small, @"small rendering did not hit the document cache");

  // Uncommitted text is independently owned by each growing snapshot. This
  // tail alone costs more than the ordinary budget, but its latest preparation
  // must still survive until measure/mount.
  const auto largeDocument = makeDocument([@"x" stringByPaddingToLength:600 * 1024 withString:@"x" startingAtIndex:0]);
  DSRenderedDocument *held = obtain(largeDocument);
  require(obtain(largeDocument) == held, @"latest oversized preparation was discarded before mount");
  const auto nextLargeDocument = makeDocument([@"y" stringByPaddingToLength:600 * 1024 withString:@"y" startingAtIndex:0]);
  DSRenderedDocument *next = obtain(nextLargeDocument);
  require(obtain(nextLargeDocument) == next, @"latest oversized preparation did not hit");
  DSRenderedDocument *rebuilt = obtain(largeDocument);
#ifdef BASELINE
  require(held == rebuilt, @"baseline did not retain the oversized historical tail");
#else
  require(held != rebuilt, @"historical oversized uncommitted tails accumulated");
#endif
  DSCodeSegment *segment = held.segments.firstObject;
  require(segment.text.attributedString.length == 600 * 1024, @"cache eviction invalidated a held rendering");
  require(obtain(smallDocument) == small, @"oversized tail displaced a small rendering");

  // A shared committed prefix is omitted from the snapshot's tail estimate.
  // It must not make a small changing tail use the oversized slot.
  auto sharedDocument = std::make_shared<downstream::Document>(*largeDocument);
  sharedDocument->id += 100000;
  sharedDocument->blocks.tail()[0].identity = 777777;
  downstream::Block tail;
  tail.kind = downstream::BlockKind::Paragraph;
  tail.text = u"streaming tail";
  sharedDocument->blocks.push_back(std::move(tail));
  DSRenderedDocument *shared = obtain(sharedDocument);
  (void)obtain(makeDocument(@"another short message"));
  require(obtain(sharedDocument) == shared, @"shared committed payload was charged to the snapshot");
  printf("rendering_cache historical_oversized_tail_cached=%d held_tail_characters=%lu small_hit=1 shared_prefix_hit=1\n",
         held == rebuilt, (unsigned long)segment.text.attributedString.length);
  [DSRenderedDocument invalidateCaches];
}

/**
 * A hosted fence: a React child replaced the block, and the document is as tall
 * as the child measured with the child's rectangle at the block's own column.
 *
 * The shadow node's half of this contract is a C++ layout pass, which this
 * standalone executable cannot run, so what is pinned here is the half the
 * platform owns: that a host layout replaces the card's height rather than
 * adding to it, that the rectangle is the content column rather than the full
 * width, and that an empty host layout measures exactly what it did before
 * fence renderers existed.
 */
static downstream::Block makeBlock(downstream::BlockKind kind, const char *utf8) {
  downstream::Block block;
  block.kind = kind;
  block.language = kind == downstream::BlockKind::CodeBlock ? "mermaid" : "";
  const std::string text(utf8);
  block.text = std::u16string(reinterpret_cast<const char16_t *>(text.data()), text.size());
  block.fenceMarker = kind == downstream::BlockKind::CodeBlock ? '`' : 0;
  block.fenceLength = 3;
  block.fenceClosed = true;
  return block;
}

static downstream::Container listItem(uint32_t id) {
  downstream::Container container;
  container.kind = downstream::ContainerKind::ListItem;
  container.id = id;
  container.showsMarker = true;
  container.marker = u"•";
  return container;
}

static void checkHostedTagCache() {
  auto document = std::make_shared<downstream::Document>();
  document->id = 9102;
  auto block = makeBlock(downstream::BlockKind::Paragraph, "Hosted content");
  downstream::Container container;
  container.kind = downstream::ContainerKind::Custom;
  container.id = 42;
  block.containers.push_back(container);
  document->blocks.push_back(std::move(block));
  document->settledBlocks = 1;
  DSRenderedDocument *rendered = [DSRenderedDocument renderedDocumentFor:document
                                                             fontScale:1 syntaxHighlighting:NO
                                                                 theme:downstream::Theme::defaultTheme()
                                                                 caret:downstream::PlacedCaret{}];
  downstream::HostLayout hosts;
  hosts.tags = {{42, 90}};
  downstream::CodeLayout code;
  downstream::ImageStateLayout images;
  downstream::TableLayout tables;
  const DSMeasurement first = [rendered measureForWidth:320 chrome:0 code:code images:images
                                                 hosts:hosts tables:tables];
  const DSMeasurement cached = [rendered measureForWidth:320 chrome:0 code:code images:images
                                                  hosts:hosts tables:tables];
  require(first.tagHosts.size() == 1, @"hosted tag produced no rectangle");
  require(cached.tagHosts.size() == 1, @"cached measurement dropped the hosted tag rectangle");
  require(cached.tagHosts[0].first == 42 && CGRectEqualToRect(first.tagHosts[0].second, cached.tagHosts[0].second),
          @"cached measurement changed the hosted tag rectangle");

  DSContainerInfo *before = [DSContainerInfo new];
  DSContainerInfo *after = [DSContainerInfo new];
  require([before isEqual:after], @"equal container metadata did not compare equally");
  for (NSString *field in @[@"containerId", @"writtenOpen", @"summary"]) {
    [after setValue:@1 forKey:field];
    require(![before isEqual:after], @"TextKit reuse ignored changed container metadata");
    [after setValue:@0 forKey:field];
  }
}

static void checkHostedFenceLayout() {
  // One paragraph, a hosted fence, and a hosted fence inside a list item: the
  // second is the case where the rectangle is not the whole column.
  auto document = std::make_shared<downstream::Document>();
  document->id = 9101;
  document->blocks.push_back(makeBlock(downstream::BlockKind::Paragraph, "Intro paragraph."));
  document->blocks.push_back(makeBlock(downstream::BlockKind::CodeBlock, "graph TD;"));
  auto inList = makeBlock(downstream::BlockKind::CodeBlock, "graph TD;");
  inList.containers.push_back(listItem(1));
  document->blocks.push_back(std::move(inList));
  document->settledBlocks = 3;

  DSRenderedDocument *rendered =
      [DSRenderedDocument renderedDocumentFor:document
                                      fontScale:1
                             syntaxHighlighting:NO
                                      theme:downstream::Theme::defaultTheme()
                                      caret:downstream::PlacedCaret{}];
  downstream::CodeLayout code;
  code.props.wrap = NO;
  code.props.lineNumbers = YES;
  code.props.maxHeight = 0;
  downstream::ImageStateLayout images;
  const CGFloat width = 320;
  downstream::HostLayout empty;
  const CGFloat cardHeight = [rendered sizeForWidth:width chrome:0 code:code images:images hosts:empty tables:downstream::TableLayout{}].height;
  // Two hosted fences, at the heights a fixed-size component measured at.
  downstream::HostLayout hosts;
  hosts.entries = {{0, 120}, {1, 80}};
  const DSMeasurement measured = [rendered measureForWidth:width chrome:0 code:code images:images hosts:hosts tables:downstream::TableLayout{}];
  require(measured.hosts.size() == 2, @"a hosted fence produced no rectangle");
  require(std::fabs(measured.size.height - [rendered sizeForWidth:width chrome:0 code:code images:images
                                                                              hosts:hosts tables:downstream::TableLayout{}]
                                             .height) < 0.001,
          @"measure and size disagree about a hosted document");
  require(measured.hosts[0].second.size.height == 120, @"a hosted fence did not take its component's height");
  require(measured.hosts[1].second.size.height == 80, @"the second hosted fence did not take its component's height");
  require(measured.hosts[0].second.origin.x == 0, @"a top-level fence's rectangle is not at the column's edge");
  require(measured.hosts[0].second.size.width == width, @"a top-level fence did not get the whole column");
  require(measured.hosts[1].second.origin.x > 0, @"a fence in a list did not move in by the list margin");
  require(measured.hosts[1].second.size.width < width, @"a fence in a list did not get the narrower column");
  // The document is shorter with the fences hosted than as two cards, because a
  // component is not a code card's height. That difference is the feature.
  // The height is the component's, not the card's: hosting the same fence with
  // a taller component makes the document taller by exactly that difference,
  // and a card would not move at all.
  downstream::HostLayout taller;
  taller.entries = {{0, 200}, {1, 80}};
  const CGFloat tallerHeight = [rendered sizeForWidth:width chrome:0 code:code images:images hosts:taller tables:downstream::TableLayout{}].height;
  require(std::fabs(tallerHeight - (measured.size.height + 80)) < 0.001,
          @"a hosted fence's height did not follow its component");
  require(tallerHeight > cardHeight, @"a tall component did not raise the document");
  const CGFloat narrowWidth = 180;
  const DSMeasurement narrow = [rendered measureForWidth:narrowWidth chrome:0 code:code images:images hosts:hosts tables:downstream::TableLayout{}];
  require(narrow.hosts[0].second.size.width == narrowWidth, @"a cached hosted rectangle kept the old width");
  require(narrow.hosts[1].second.size.width == narrowWidth - narrow.hosts[1].second.origin.x,
          @"a nested hosted rectangle lost its content column");
  const auto indentedTheme = downstream::ThemeRegistry::intern({downstream::theme::kSlotListMarginLeft}, {48}, {});
  DSRenderedDocument *themed = [DSRenderedDocument renderedDocumentFor:document fontScale:1 syntaxHighlighting:NO
                                                               theme:indentedTheme caret:downstream::PlacedCaret{}];
  const DSMeasurement themedMeasure = [themed measureForWidth:width chrome:0 code:code images:images hosts:hosts tables:downstream::TableLayout{}];
  const CGFloat indentDelta = 48 - downstream::Theme::defaultTheme()->number(downstream::theme::kSlotListMarginLeft);
  require(themedMeasure.hosts[1].second.origin.x == measured.hosts[1].second.origin.x + indentDelta,
          @"a themed hosted rectangle kept the default list indent");
  require(CGRectGetMaxX(themedMeasure.hosts[1].second) == width &&
              themedMeasure.hosts[1].second.size.height == measured.hosts[1].second.size.height,
          @"a theme changed hosted height or placed its content outside the column");
  downstream::HostLayout blank;
  blank.entries = {{0, 0}, {99, 100}};
  const DSMeasurement zero = [rendered measureForWidth:width chrome:0 code:code images:images hosts:blank tables:downstream::TableLayout{}];
  require(zero.hosts.size() == 1 && zero.hosts[0].first == 0,
          @"an absent fence produced a rectangle or a zero-height fence lost its rectangle");
  require(zero.hosts[0].second.size.height == 0, @"a zero-height hosted fence drew a code card");
  const DSMeasurement restored = [rendered measureForWidth:width chrome:0 code:code images:images hosts:hosts tables:downstream::TableLayout{}];
  require(restored.size.height == measured.size.height && restored.hosts.size() == measured.hosts.size(),
          @"changing hosted ordinals reused stale geometry");
#ifndef BASELINE
  // Each result must describe its own inputs even while another thread replaces
  // the shared one-entry cache with different heights and widths.
  std::atomic<bool> consistent{true};
  auto *consistentResult = &consistent;
  dispatch_apply(1024, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^(size_t index) {
    @autoreleasepool {
      downstream::HostLayout localHosts;
      const CGFloat localHeight = 40 + index % 17;
      const CGFloat localWidth = 180 + index % 31;
      localHosts.entries = {{0, static_cast<float>(localHeight)}, {1, 80}};
      const DSMeasurement result = [rendered measureForWidth:localWidth chrome:0 code:code images:images
                                                   hosts:localHosts tables:downstream::TableLayout{}];
      if (result.hosts.size() != 2 || result.hosts[0].second.size.height != localHeight ||
          result.hosts[0].second.size.width != localWidth ||
          result.size.height != CGRectGetMaxY(result.hosts.back().second)) {
        consistentResult->store(false);
      }
    }
  });
  require(consistent.load(), @"concurrent document measurement mixed size and rectangles from different inputs");
#endif
  // An empty host layout is the measurement this view made before fence
  // renderers existed: same size, same frames.
  const NSArray<NSValue *> *plainFrames =
      [rendered segmentFramesForWidth:width chrome:0 code:code images:images hosts:empty tables:downstream::TableLayout{}];
  const NSArray<NSValue *> *emptyFrames = [rendered segmentFramesForWidth:width chrome:0 code:code images:images
                                                                    hosts:downstream::HostLayout{} tables:downstream::TableLayout{}];
  require(plainFrames.count == emptyFrames.count, @"an empty host layout changed the frame count");
  for (NSUInteger index = 0; index < plainFrames.count; index++) {
    require(CGRectEqualToRect(plainFrames[index].CGRectValue, emptyFrames[index].CGRectValue),
            @"an empty host layout changed a segment frame");
  }
  printf("ios_hosted_fence hosted_height=%.1f card_height=%.1f rectangles=%zu\n", measured.size.height, cardHeight,
         measured.hosts.size());
}

static void benchmarkHostedMeasurements() {
  constexpr uint32_t fences = 512;
  constexpr size_t iterations = 5000;
  auto document = std::make_shared<downstream::Document>();
  document->id = 9102;
  downstream::HostLayout hosts;
  for (uint32_t ordinal = 0; ordinal < fences; ++ordinal) {
    document->blocks.push_back(makeBlock(downstream::BlockKind::CodeBlock, "graph TD;"));
    hosts.entries.push_back({ordinal, 50});
  }
  document->settledBlocks = fences;
  DSRenderedDocument *rendered = [DSRenderedDocument renderedDocumentFor:document fontScale:1
                                                  syntaxHighlighting:NO theme:downstream::Theme::defaultTheme()
                                                               caret:downstream::PlacedCaret{}];
  const downstream::CodeLayout code;
  const downstream::ImageStateLayout images;
  (void)[rendered measureForWidth:320 chrome:0 code:code images:images hosts:hosts tables:downstream::TableLayout{}];
  CGFloat checksum = 0;
  const auto sizeStart = std::chrono::steady_clock::now();
  for (size_t index = 0; index < iterations; ++index) {
    checksum += [rendered sizeForWidth:320 chrome:0 code:code images:images hosts:hosts tables:downstream::TableLayout{}].height;
  }
  const auto measureStart = std::chrono::steady_clock::now();
  for (size_t index = 0; index < iterations; ++index) {
    const DSMeasurement result = [rendered measureForWidth:320 chrome:0 code:code images:images hosts:hosts tables:downstream::TableLayout{}];
    checksum += result.size.height + result.hosts.size();
  }
  const auto alternatingStart = std::chrono::steady_clock::now();
  for (size_t index = 0; index < iterations; ++index) {
    const DSMeasurement result = [rendered measureForWidth:index % 2 ? 240 : 320 chrome:0 code:code images:images
                                                   hosts:hosts tables:downstream::TableLayout{}];
    checksum += result.size.height + result.hosts.size();
  }
  const auto end = std::chrono::steady_clock::now();
  const auto micros = [](auto start, auto finish) {
    return std::chrono::duration<double, std::micro>(finish - start).count() / iterations;
  };
  require(checksum > 0, @"hosted measurement benchmark produced no geometry");
  printf("ios_host_measurement fences=%u iterations=%zu size_hit_us=%.3f measure_hit_us=%.3f "
         "measure_alternating_us=%.3f checksum=%.0f\n", fences, iterations,
         micros(sizeStart, measureStart), micros(measureStart, alternatingStart), micros(alternatingStart, end), checksum);
}

/// A paragraph holding a script run against the same prose without one.
///
/// Streamdown's line height does not grow for a superscript, because Tailwind's
/// preflight gives `sup` `line-height: 0`. TextKit grows a line fragment to fit a
/// baseline offset, so this is where that claim is measured rather than assumed:
/// a raised run that opened its line by more than half a point would be the case
/// the paragraph cap exists for.
static void checkScriptLineHeight() {
  const char16_t prose[] = u"E=mc2 and H2O";
  const auto documentFor = [](const std::u16string &text, const std::vector<downstream::InlineRun> &runs) {
    static uint64_t nextId = 9400;
    auto document = std::make_shared<downstream::Document>();
    document->id = nextId++;
    downstream::Block block;
    block.kind = downstream::BlockKind::Paragraph;
    block.text = text;
    block.runs = runs;
    document->blocks.push_back(std::move(block));
    return document;
  };
  DSRenderedDocument *plain = [DSRenderedDocument renderedDocumentFor:documentFor(std::u16string(prose), {})
                                                              fontScale:1
                                                     syntaxHighlighting:NO
                                                                  theme:downstream::Theme::defaultTheme()
                                                                caret:downstream::PlacedCaret{}];
  DSRenderedDocument *raised =
      [DSRenderedDocument renderedDocumentFor:documentFor(std::u16string(prose),
                                                          {{2, 3, downstream::InlineStyle::Superscript},
                                                           {7, 8, downstream::InlineStyle::Subscript}})
                                        fontScale:1
                               syntaxHighlighting:NO
                                        theme:downstream::Theme::defaultTheme()
                                                                caret:downstream::PlacedCaret{}];
  DSTextSegment *plainSegment = (DSTextSegment *)plain.segments.firstObject;
  DSTextSegment *raisedSegment = (DSTextSegment *)raised.segments.firstObject;
  require(plainSegment != nil && raisedSegment != nil, @"paragraph did not produce a text segment");
  const CGFloat plainHeight = [plainSegment heightForWidth:CGFLOAT_MAX];
  const CGFloat raisedHeight = [raisedSegment heightForWidth:CGFLOAT_MAX];
  require(plainHeight > 0, @"a plain paragraph measured nothing");
  printf("ios_script_line_height plain=%.1f with_scripts=%.1f\n", plainHeight, raisedHeight);
  if (fabs(raisedHeight - plainHeight) > 0.5) {
    fprintf(stderr, "FAIL: a script run opened its line by %.2f; the paragraph cap would be needed here\n",
            raisedHeight - plainHeight);
    exit(1);
  }
}

/// Each line's baseline in a two-line paragraph whose first line holds a script,
/// against the same paragraph without one.
///
/// The cap above holds a paragraph's height, but TextKit sets a capped line's
/// baseline from its deepest descent, so a lowered run used to lift its own line
/// toward the block above and leave a wider gap below. Heights alone cannot see
/// that; baselines can.
static void checkScriptBaselines() {
  const std::u16string prose = u"Sub y2 sup x2\nnext line";
  const auto baselinesFor = [&](const std::vector<downstream::InlineRun> &runs) {
    static uint64_t nextId = 9600;
    auto document = std::make_shared<downstream::Document>();
    document->id = nextId++;
    downstream::Block block;
    block.kind = downstream::BlockKind::Paragraph;
    block.text = prose;
    block.runs = runs;
    document->blocks.push_back(std::move(block));
    DSRenderedDocument *rendered = [DSRenderedDocument renderedDocumentFor:document
                                                                  fontScale:1
                                                         syntaxHighlighting:NO
                                                                      theme:downstream::Theme::defaultTheme()
                                                                      caret:downstream::PlacedCaret{}];
    DSTextSegment *segment = (DSTextSegment *)rendered.segments.firstObject;
    require(segment != nil, @"paragraph did not produce a text segment");
    DSTextKitStack *stack = [[DSTextKitStack alloc] initWithLayoutManager:[NSLayoutManager new]];
    stack.textContainer.size = CGSizeMake(CGFLOAT_MAX, CGFLOAT_MAX);
    [stack.textStorage setAttributedString:segment.attributedString];
    [stack.layoutManager ensureLayoutForTextContainer:stack.textContainer];
    std::vector<CGFloat> baselines;
    for (const NSUInteger character : {NSUInteger{0}, NSUInteger{14}}) {
      const NSUInteger glyph = [stack.layoutManager glyphIndexForCharacterAtIndex:character];
      const CGRect fragment = [stack.layoutManager lineFragmentRectForGlyphAtIndex:glyph effectiveRange:NULL];
      baselines.push_back(CGRectGetMinY(fragment) + [stack.layoutManager locationForGlyphAtIndex:glyph].y);
    }
    return baselines;
  };
  const auto plain = baselinesFor({});
  const auto scripted = baselinesFor({{5, 6, downstream::InlineStyle::Subscript},
                                      {12, 13, downstream::InlineStyle::Superscript}});
  printf("ios_script_baselines plain=%.1f,%.1f with_scripts=%.1f,%.1f\n", plain[0], plain[1], scripted[0],
         scripted[1]);
  for (size_t line = 0; line < plain.size(); line++) {
    if (fabs(plain[line] - scripted[line]) > 0.5) {
      fprintf(stderr, "FAIL: a script moved line %zu's baseline by %.2f\n", line + 1,
              scripted[line] - plain[line]);
      exit(1);
    }
  }
}

/// A cell holding a `<br>` measures two lines, which is what makes a table row the
/// only place a model can break a line.
static void checkCellBreakHeight() {
  static uint64_t nextId = 9500;
  auto document = std::make_shared<downstream::Document>();
  document->id = nextId++;
  downstream::Block header;
  header.kind = downstream::BlockKind::TableRow;
  header.tableHeader = true;
  header.table = 1;
  header.text = u"Step\tNotes";
  header.cells = {{0, 4}, {5, 10}};
  downstream::Block row;
  row.kind = downstream::BlockKind::TableRow;
  row.table = 1;
  row.text = u"1\tMix\nThen";
  row.cells = {{0, 1}, {2, 10}};
  downstream::Block flat;
  flat.kind = downstream::BlockKind::TableRow;
  flat.table = 1;
  // One word, so the control row cannot wrap and the difference can only be
  // the break.
  flat.text = u"1\tMix";
  flat.cells = {{0, 1}, {2, 5}};
  document->blocks.push_back(std::move(header));
  document->blocks.push_back(std::move(row));
  DSRenderedDocument *broken = [DSRenderedDocument renderedDocumentFor:document
                                                              fontScale:1
                                                     syntaxHighlighting:NO
                                                                  theme:downstream::Theme::defaultTheme()
                                                                caret:downstream::PlacedCaret{}];
  auto without = std::make_shared<downstream::Document>(*document);
  without->id = 9501;
  without->blocks.tail().clear();
  // A different header row, because a table's rendering is cached under its
  // first row's text: the control must be a table of its own.
  downstream::Block controlHeader;
  controlHeader.kind = downstream::BlockKind::TableRow;
  controlHeader.tableHeader = true;
  controlHeader.table = 2;
  controlHeader.text = u"Step\tNote";
  controlHeader.cells = {{0, 4}, {5, 9}};
  without->blocks.push_back(std::move(controlHeader));
  without->blocks.push_back(std::move(flat));
  DSRenderedDocument *plain = [DSRenderedDocument renderedDocumentFor:without
                                                               fontScale:1
                                                      syntaxHighlighting:NO
                                                                   theme:downstream::Theme::defaultTheme()
                                                                caret:downstream::PlacedCaret{}];
  DSTableSegment *brokenTable = (DSTableSegment *)broken.segments.firstObject;
  DSTableSegment *plainTable = (DSTableSegment *)plain.segments.firstObject;
  require(brokenTable != nil && plainTable != nil, @"the table did not produce a table segment");
  require(brokenTable.rows.count == 2, ([NSString stringWithFormat:@"the table lost a row: %lu",
                                              (unsigned long)brokenTable.rows.count]));
  const CGFloat brokenHeight = brokenTable.rowHeights.lastObject.doubleValue;
  const CGFloat plainHeight = plainTable.rowHeights.lastObject.doubleValue;
  require(brokenHeight > plainHeight,
          ([NSString stringWithFormat:@"a cell break did not make its row taller: broken %.1f plain %.1f",
                                    brokenHeight, plainHeight]));
  DSTableSegmentView *tableView = [DSTableSegmentView new];
  [tableView setSegment:brokenTable];
  tableView.frame = CGRectMake(0, 0, 120, brokenTable.tableSize.height);
  [tableView layoutIfNeeded];
  UIScrollView *viewport = [tableView valueForKey:@"scrollView"];
  UIView *grid = [tableView valueForKey:@"grid"];
  UIView *pinnedHeader = [tableView valueForKey:@"header"];
  const CGFloat headerHeight = brokenTable.rowHeights.firstObject.doubleValue;
  require(grid.frame.origin.y == -headerHeight, @"scrolling table repeated its pinned header");
  require(viewport.contentSize.height == brokenTable.tableSize.height - headerHeight,
          @"scrolling table retained the pinned row in its content height");
  viewport.contentOffset = CGPointMake(20, 0);
  [tableView layoutIfNeeded];
  require(pinnedHeader.frame.origin.x == viewport.frame.origin.x && pinnedHeader.bounds.origin.x == 20,
          @"pinned table columns did not track horizontal scrolling");
  printf("ios_table_cell_break row_broken=%.1f row_plain=%.1f\n", brokenHeight, plainHeight);
}

#include "HighlightVariants.mm"

#include "TextTailChecks.mm"

#include "DetailsChecks.mm"

#import "DSSegmentViewport.h"
#include "PerformanceChecks.mm"
#ifndef BASELINE
#include "SnapshotStringChecks.mm"
#include "RetirementChecks.mm"
#import <QuartzCore/QuartzCore.h>
#include "DownstreamSessionFrameClock.h"
#include "FrameScheduler.h"

static void runClockLoopUntil(const std::function<bool()> &done) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!done() && std::chrono::steady_clock::now() < deadline) {
    [[NSRunLoop mainRunLoop] runMode:NSDefaultRunLoopMode beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.002]];
  }
  require(done(), @"UIKit session clock did not make progress before timeout");
}

static void checkSessionFrameClock() {
  require(NSThread.isMainThread, @"UIKit clock test must run on the main thread");
  const BOOL wasActive = UIApplication.sharedApplication.applicationState == UIApplicationStateActive;
  downstream::installSessionFrameClock();
  id clock = [NSClassFromString(@"DownstreamSessionFrameClock") performSelector:@selector(shared)];
  require(clock != nil, @"UIKit session clock was not linked into the harness");
  NSNotificationCenter *notifications = NSNotificationCenter.defaultCenter;
  [notifications postNotificationName:UIApplicationDidBecomeActiveNotification object:nil];

  NSUInteger completed = 0;
  double firstTimestamp = 0;
  double secondTimestamp = 0;
  double displayInterval = 0;
  downstream::FrameScheduler::post([&](const downstream::FrameInfo &info) {
    require(NSThread.isMainThread, @"display clock ran a frame callback off the main thread");
    CADisplayLink *link = [clock valueForKey:@"displayLink"];
    require(link != nil && link.paused, @"display clock did not pause its one-shot tick");
    firstTimestamp = link.timestamp;
    displayInterval = (link.targetTimestamp - link.timestamp) * 1000;
    require(std::isfinite(info.intervalMs) && info.intervalMs > 0 &&
            fabs(info.intervalMs - displayInterval) < 0.001,
            @"UIKit frame callback did not receive the actual display interval");
    completed++;
    downstream::FrameScheduler::post([&](const downstream::FrameInfo &nextInfo) {
      CADisplayLink *nextLink = [clock valueForKey:@"displayLink"];
      secondTimestamp = nextLink.timestamp;
      require(NSThread.isMainThread && nextInfo.intervalMs > 0 && secondTimestamp > firstTimestamp,
              @"reentrant frame work ran before the following actual display tick");
      completed++;
    });
    require(completed == 1, @"reentrant frame work ran inline");
  });
  require(completed == 0, @"UIKit frame requester ran posted work inline");
  runClockLoopUntil([&] { return completed == 2; });
  CADisplayLink *link = [clock valueForKey:@"displayLink"];
  require(link.paused, @"empty UIKit session clock kept ticking");

  NSUInteger pendingCompleted = 0;
  downstream::FrameScheduler::post([&](const downstream::FrameInfo &) { pendingCompleted++; });
  // Arm the real driver before yielding the run loop, so resigning active is
  // guaranteed to interrupt a pending display tick rather than an idle link.
  [clock performSelector:@selector(requestFrame)];
  require(!link.paused && pendingCompleted == 0, @"pending display request did not arm asynchronously");
  [notifications postNotificationName:UIApplicationWillResignActiveNotification object:nil];
  require(link.paused, @"inactive transition left the pending display link running");
  runClockLoopUntil([&] { return pendingCompleted == 1; });
  require(link.paused, @"inactive fallback woke the display link");

  NSUInteger inactiveCompleted = 0;
  downstream::FrameScheduler::post([&](const downstream::FrameInfo &info) {
    require(NSThread.isMainThread && std::isfinite(info.intervalMs) && info.intervalMs > 0,
            @"inactive clock fallback lost its frame callback");
    inactiveCompleted++;
  });
  runClockLoopUntil([&] { return inactiveCompleted == 1; });
  require(link.paused, @"inactive post enabled display ticking");

  NSUInteger resumedCompleted = 0;
  [notifications postNotificationName:UIApplicationDidBecomeActiveNotification object:nil];
  downstream::FrameScheduler::post([&](const downstream::FrameInfo &info) {
    require(link.timestamp > secondTimestamp &&
            fabs(info.intervalMs - (link.targetTimestamp - link.timestamp) * 1000) < 0.001,
            @"resumed clock did not return to actual display ticks");
    resumedCompleted++;
  });
  runClockLoopUntil([&] { return resumedCompleted == 1; });
  require(link.paused && pendingCompleted == 1 && inactiveCompleted == 1 && completed == 2,
          @"clock replayed work or kept running after queues drained");
  if (!wasActive) [notifications postNotificationName:UIApplicationWillResignActiveNotification object:nil];
  downstream::FrameScheduler::install({});
  printf("ios_session_frame_clock display_interval_ms=%.3f next_tick_reentrant=1 idle_paused=1 pending_inactive_progress=1 inactive_post=1 resumed_display_tick=1\n",
         displayInterval);
}

static std::shared_ptr<downstream::Document> finishReuseDocument(const downstream::DocumentHandle &live) {
  static uint64_t nextId = 9860000;
  auto finished = std::make_shared<downstream::Document>(*live);
  finished->id = nextId++;
  finished->streaming = false;
  finished->settledBlocks = finished->blocks.size();
  // The fixture changes only lifecycle metadata. The engine tests own proof
  // that a real finish may publish this hint for equivalent render payload.
  finished->renderReuseFrom = live->id;
  return finished;
}

static void checkFinishRenderingReuse() {
  [DSRenderedDocument invalidateCaches];
  auto live = std::make_shared<downstream::Document>(*makeDocument(@"keyword\nlast line"));
  live->streaming = true;
  live->blocks.tail()[0].tokens.push_back({0, 7, downstream::TokenClass::Keyword});
  downstream::Block header;
  header.kind = downstream::BlockKind::TableRow;
  header.table = 1;
  header.tableHeader = true;
  header.text = u"Name\tValue";
  header.cells = {{0, 4}, {5, 10}};
  live->blocks.push_back(header);
  downstream::Block row;
  row.kind = downstream::BlockKind::TableRow;
  row.table = 1;
  row.text = u"tail\t1";
  row.cells = {{0, 4}, {5, 6}};
  live->blocks.push_back(row);
  require(live->settledBlocks == 0 && live->blocks[0].identity == 0 && live->blocks[2].identity == 0,
          @"finish reuse fixture must have an uncommitted identity-zero tail");

  const auto render = [](const downstream::DocumentHandle &document) {
    return [DSRenderedDocument renderedDocumentFor:document fontScale:1 syntaxHighlighting:NO
                                             theme:downstream::Theme::defaultTheme() caret:downstream::PlacedCaret{}];
  };
  DSRenderedDocument *before = render(live);
  require(before.segments.count == 2 && [before.segments[0] isKindOfClass:DSCodeSegment.class] &&
          [before.segments[1] isKindOfClass:DSTableSegment.class], @"finish reuse fixture lost its code/table segments");
  const downstream::CodeLayout code;
  const downstream::ImageStateLayout images;
  const downstream::HostLayout hosts;
  const downstream::TableLayout tables;
  NSArray<NSValue *> *beforeFrames = [before segmentFramesForWidth:320 chrome:0 code:code images:images hosts:hosts tables:tables];
  DSDocumentView *view = [[DSDocumentView alloc] initWithFrame:CGRectMake(0, 0, 320, 300)];
  view.rendered = before;
  NSArray *mounted = [view valueForKey:@"segmentViews"];
  DSCodeSegmentView *codeView = mounted[0];
  DSTableSegmentView *tableView = mounted[1];
  require(!codeView.controlsEnabled && codeView.streaming && !tableView.controlsEnabled && tableView.streaming,
          @"live identity-zero code/table did not keep controls disabled");

  const auto finished = finishReuseDocument(live);
  DSRenderedDocument *after = render(finished);
  require(after != before && after.documentId == finished->id && before.documentId == live->id,
          @"finish reuse aliased the wrapper or retained the old document id");
  require(before.settledBlocks == 0 && after.settledBlocks == finished->blocks.size(),
          @"finish reuse retained live settlement or changed the old wrapper");
  require(after.segments == before.segments && after.segmentBlockIndexes == before.segmentBlockIndexes &&
          after.theme == before.theme && after.caret == before.caret,
          @"finish reuse rebuilt immutable segments or lost rendering metadata");
  require([after valueForKey:@"lock"] != [before valueForKey:@"lock"], @"finish reuse shared a mutable geometry lock");
  NSArray<NSValue *> *afterFrames = [after segmentFramesForWidth:320 chrome:0 code:code images:images hosts:hosts tables:tables];
  require(afterFrames != beforeFrames && [afterFrames isEqualToArray:beforeFrames],
          @"finish reuse shared the wrapper's geometry cache or changed its frames");
  require(render(finished) == after, @"finished wrapper did not enter the document cache");
  view.rendered = after;
  mounted = [view valueForKey:@"segmentViews"];
  require(mounted[0] == codeView && mounted[1] == tableView &&
          codeView.controlsEnabled && !codeView.streaming && tableView.controlsEnabled && !tableView.streaming &&
          codeView.blockOffset == 0 && tableView.blockOffset == 1,
          @"shared finish segments skipped the document view's settled code/table state");
  require(render(finished) == after, @"mount retirement evicted the finished wrapper");
  view.rendered = nil;

  // Retained owners stay valid, but cache misses must build from the finished
  // document rather than treating its scalar hint as an owned parent.
  DSRenderedDocument *retiredFallback = render(finishReuseDocument(live));
  require(retiredFallback.segments[0] != before.segments[0] && retiredFallback.settledBlocks == live->blocks.size(),
          @"retired parent was reused or fallback lost final settlement");
  DSRenderedDocument *evictionParent = render(live);
  NSCache *ordinaryCache = [[DSRenderedDocument cache] valueForKey:@"cache"];
  [ordinaryCache removeAllObjects];
  DSRenderedDocument *evictedFallback = render(finishReuseDocument(live));
  require(evictedFallback.segments[0] != evictionParent.segments[0] &&
          [((DSCodeSegment *)evictedFallback.segments[0]).text.attributedString
              isEqualToAttributedString:((DSCodeSegment *)before.segments[0]).text.attributedString],
          @"evicted parent fallback reused stale segments or changed code payload");
  printf("ios_finish_rendering_reuse shared_segments=2 distinct_wrapper=1 settled_controls=1 own_geometry=1 retired_fallback=1 evicted_fallback=1\n");
  [DSRenderedDocument invalidateCaches];
}

static void checkFinishReuseOptionMismatches() {
  [DSRenderedDocument invalidateCaches];
  const auto live = makeDocument(@"keyword");
  const auto theme = downstream::Theme::defaultTheme();
  const downstream::PlacedCaret none;
  DSRenderedDocument *before = [DSRenderedDocument renderedDocumentFor:live fontScale:1 syntaxHighlighting:NO
                                                                 theme:theme caret:none];
  const auto check = [&](CGFloat scale, BOOL highlighting, const downstream::Theme::Handle &optionTheme,
                         const downstream::PlacedCaret &caret, DSLinkVariants *variants, DSTagStyles *tagStyles) {
    const auto finished = finishReuseDocument(live);
    DSRenderedDocument *rendered = [DSRenderedDocument renderedDocumentFor:finished fontScale:scale
                                                       syntaxHighlighting:highlighting theme:optionTheme caret:caret
                                                                  variants:variants tagStyles:tagStyles];
    require(rendered.segments.firstObject != before.segments.firstObject &&
            rendered.documentId == finished->id && rendered.settledBlocks == 1 && rendered.caret == caret,
            @"finish reused a parent with different render options or lost final state");
  };
  check(1.25, NO, theme, none, nil, nil);
  check(1, YES, theme, none, nil, nil);
  const auto coloredTheme = downstream::Theme::create({downstream::theme::kSlotCodeBlockColor}, {0xff336699}, {}, 9861000, 9861000);
  check(1, NO, coloredTheme, none, nil, nil);
  check(1, NO, theme, {downstream::CaretKind::Block, downstream::CaretPlace::AfterText}, nil, nil);
  const auto caretLive = makeDocument(@"caret tail");
  const downstream::PlacedCaret inlineBlock{downstream::CaretKind::Block, downstream::CaretPlace::AfterText};
  DSRenderedDocument *caretBefore = [DSRenderedDocument renderedDocumentFor:caretLive fontScale:1
                                                        syntaxHighlighting:NO theme:theme caret:inlineBlock];
  for (const downstream::PlacedCaret caret : {downstream::PlacedCaret{downstream::CaretKind::Circle, downstream::CaretPlace::AfterText},
                                              downstream::PlacedCaret{downstream::CaretKind::Block, downstream::CaretPlace::OwnLine}}) {
    DSRenderedDocument *caretAfter = [DSRenderedDocument renderedDocumentFor:finishReuseDocument(caretLive) fontScale:1
                                                         syntaxHighlighting:NO theme:theme caret:caret];
    require(caretAfter.segments.firstObject != caretBefore.segments.firstObject && caretAfter.caret == caret,
            @"finish reused a parent across a caret kind-only or place-only change");
  }
  DSLinkVariants *variants = [DSLinkVariants new];
  variants.prefixes = @[@"https://example.com/"];
  variants.counts = @[@1];
  variants.colors = @[@(0xff336699)];
  variants.underlines = @[@YES];
  check(1, NO, theme, none, variants, nil);
  DSTagStyles *tagStyles = [DSTagStyles new];
  tagStyles.names = @[@"mark"];
  tagStyles.colors = @[@(0xff336699)];
  tagStyles.flags = @[@1];
  check(1, NO, theme, none, nil, tagStyles);
  printf("ios_finish_reuse_options scale=1 highlighting=1 theme=1 caret_kind_and_place=1 variants=1 tag_styles=1\n");
  [DSRenderedDocument invalidateCaches];
}
#endif

int main() {
  @autoreleasepool {
    checkRenderingCacheBudget();
    checkHighlightVariants();
#ifndef BASELINE
    checkSessionFrameClock();
    checkTableCellReuse();
    checkRenderingRetirement();
    checkFinishRenderingReuse();
    checkFinishReuseOptionMismatches();
    checkSnapshotStrings();
    checkExactStackWidths();
    checkLazyHostedFallback();
    checkVisibleLineMarks();
    checkUnchangedSegmentAssignments();
    checkSegmentViewport();
#endif
    if (getenv("DOWNSTREAM_IOS_HOST_BENCH_ONLY") != nullptr) {
      benchmarkHostedMeasurements();
      printf("native TextKit checks passed\n");
      return 0;
    }
    NSMutableString *source = [NSMutableString stringWithString:@"😀\n\nvalue\n"];
    for (NSUInteger line = 0; line < 4096; ++line) {
      [source appendFormat:@"line-%lu xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\n", (unsigned long)line];
    }
    downstream::CodeLayout code;
    code.props.wrap = NO;
    code.props.lineNumbers = YES;
    code.props.maxHeight = 0;
    DSRenderedDocument *rendered = [DSRenderedDocument renderedDocumentFor:makeDocument(source)
                                                                  fontScale:1
                                                         syntaxHighlighting:NO
                                                                      theme:downstream::Theme::defaultTheme()
                                                              caret:downstream::PlacedCaret{}];
    DSCodeSegment *segment = rendered.segments.firstObject;
    require(segment != nil, @"code document did not produce a code segment");
#ifndef BASELINE
    require([segment baselineCharacterOffsetAtOrAfter:0] == 0, @"emoji first line baseline moved");
#endif
#ifndef BASELINE
    require([segment baselineCharacterOffsetAtOrAfter:3] == 4, @"blank-line baseline lookup skipped value line");
#endif
    checkDetailsVisibility();
    checkGrowingTextTail();
    checkEmojiGlyphMapping();
    checkBlankSuffixDraw();
    checkScriptLineHeight();
    checkScriptBaselines();
    checkCellBreakHeight();
#ifndef BASELINE
    checkGutterOwnerLifetime();
#endif
    const CGFloat unwrappedWidth = [segment.text usedWidthForWidth:CGFLOAT_MAX];
    const CGFloat wrappedWidth = [segment.text usedWidthForWidth:240];
    const CGFloat unwrappedHeight = [segment.text heightForWidth:CGFLOAT_MAX];
    const CGFloat unwrappedHeightAgain = [segment.text heightForWidth:CGFLOAT_MAX];
    require(unwrappedWidth > wrappedWidth, @"unwrapped code did not preserve long-line width");
    require(unwrappedHeight == unwrappedHeightAgain, @"alternating width cache changed unwrapped geometry");
    Method ensureMethod = class_getInstanceMethod([NSLayoutManager class], @selector(ensureLayoutForTextContainer:));
    originalEnsureLayout = method_setImplementation(ensureMethod, (IMP)countedEnsureLayout);
    layoutCalls = 0;
    const auto start = std::chrono::steady_clock::now();
    constexpr NSUInteger iterations = 12;
    for (NSUInteger i = 0; i < iterations; ++i) {
      (void)[segment.text heightForWidth:(i & 1) == 0 ? 240 : CGFLOAT_MAX];
    }
    const auto end = std::chrono::steady_clock::now();
    const double milliseconds = std::chrono::duration<double, std::milli>(end - start).count();
    printf("ios_native_code_layout alternating_width_ms=%.3f lines=%lu width_unwrapped=%.1f width_wrapped=%.1f\n",
           milliseconds, (unsigned long)segment.lineCount, unwrappedWidth, wrappedWidth);
    printf("alternating_width_layout_calls=%lu\n", (unsigned long)layoutCalls.load());
#ifdef BASELINE
    require(layoutCalls >= iterations, @"baseline did not reproduce width-cache thrashing");
#else
    require(layoutCalls == 0, @"warmed alternating widths repeated TextKit layout");
#endif
    const CGFloat widths[] = {CGFLOAT_MAX, 240, 320};
    CGFloat heights[3];
    for (size_t index = 0; index < 3; index++) heights[index] = [segment.text heightForWidth:widths[index]];
#ifndef BASELINE
    layoutCalls = 0;
    std::atomic<bool> consistent{true};
    auto *consistentResult = &consistent;
    const CGFloat *cachedWidths = widths;
    const CGFloat *cachedHeights = heights;
    dispatch_apply(64, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^(size_t index) {
      if ([segment.text heightForWidth:cachedWidths[index % 3]] != cachedHeights[index % 3]) consistentResult->store(false);
    });
    require(consistent.load() && layoutCalls == 0, @"concurrent cached measurements changed geometry or repeated layout");
#endif
    // Concurrent reads can reorder the LRU. Establish which entry is oldest
    // before verifying that a fourth distinct width evicts it.
    for (CGFloat width : widths) (void)[segment.text heightForWidth:width];
    layoutCalls = 0;
    (void)[segment.text heightForWidth:440];
    (void)[segment.text heightForWidth:CGFLOAT_MAX];
    require(layoutCalls >= 2, @"measurement cache exceeded its three-width bound");
    method_setImplementation(ensureMethod, originalEnsureLayout);
    checkHostedTagCache();
    checkHostedFenceLayout();
    benchmarkHostedMeasurements();
    printf("native TextKit checks passed\n");
  }
  return 0;
}
