#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#import "DSRenderedDocument.h"
#import "DSSegmentViews.h"
#import <objc/runtime.h>
#include "Document.h"
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
    printf("native TextKit checks passed\n");
  }
  return 0;
}
