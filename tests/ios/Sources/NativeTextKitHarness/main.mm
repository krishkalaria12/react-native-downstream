#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#import "DSRenderedDocument.h"
#import "DSSegmentViews.h"
#import <objc/runtime.h>
#include "Document.h"
#include "HostLayout.h"
#include "theme/Theme.h"
#include <chrono>
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
                                                                    theme:downstream::Theme::defaultTheme()];
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
                                                                fontScale:1 syntaxHighlighting:NO theme:downstream::Theme::defaultTheme()];
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
                                                                  fontScale:1 syntaxHighlighting:NO theme:downstream::Theme::defaultTheme()];
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
                                      theme:downstream::Theme::defaultTheme()];
  downstream::CodeLayout code;
  code.props.wrap = NO;
  code.props.lineNumbers = YES;
  code.props.maxHeight = 0;
  downstream::ImageStateLayout images;
  const CGFloat width = 320;
  downstream::HostLayout empty;
  const CGFloat cardHeight = [rendered sizeForWidth:width chrome:0 code:code images:images hosts:empty].height;
  // Two hosted fences, at the heights a fixed-size component measured at.
  downstream::HostLayout hosts;
  hosts.entries = {{0, 120}, {1, 80}};
  const DSMeasurement measured = [rendered measureForWidth:width chrome:0 code:code images:images hosts:hosts];
  require(measured.hosts.size() == 2, @"a hosted fence produced no rectangle");
  require(std::fabs(measured.size.height - [rendered sizeForWidth:width chrome:0 code:code images:images
                                                                              hosts:hosts]
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
  const CGFloat tallerHeight = [rendered sizeForWidth:width chrome:0 code:code images:images hosts:taller].height;
  require(std::fabs(tallerHeight - (measured.size.height + 80)) < 0.001,
          @"a hosted fence's height did not follow its component");
  require(tallerHeight > cardHeight, @"a tall component did not raise the document");
  // An empty host layout is the measurement this view made before fence
  // renderers existed: same size, same frames.
  const NSArray<NSValue *> *plainFrames =
      [rendered segmentFramesForWidth:width chrome:0 code:code images:images hosts:empty];
  const NSArray<NSValue *> *emptyFrames = [rendered segmentFramesForWidth:width chrome:0 code:code images:images
                                                                    hosts:downstream::HostLayout{}];
  require(plainFrames.count == emptyFrames.count, @"an empty host layout changed the frame count");
  for (NSUInteger index = 0; index < plainFrames.count; index++) {
    require(CGRectEqualToRect(plainFrames[index].CGRectValue, emptyFrames[index].CGRectValue),
            @"an empty host layout changed a segment frame");
  }
  printf("ios_hosted_fence hosted_height=%.1f card_height=%.1f rectangles=%zu\n", measured.size.height, cardHeight,
         measured.hosts.size());
}

int main() {
  @autoreleasepool {
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
                                                                      theme:downstream::Theme::defaultTheme()];
    DSCodeSegment *segment = rendered.segments.firstObject;
    require(segment != nil, @"code document did not produce a code segment");
#ifndef BASELINE
    require([segment baselineCharacterOffsetAtOrAfter:0] == 0, @"emoji first line baseline moved");
#endif
#ifndef BASELINE
    require([segment baselineCharacterOffsetAtOrAfter:3] == 4, @"blank-line baseline lookup skipped value line");
#endif
    checkEmojiGlyphMapping();
    checkBlankSuffixDraw();
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
    checkHostedFenceLayout();
    printf("native TextKit checks passed\n");
  }
  return 0;
}
