#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#import "DSRenderedDocument.h"
#import "DSSegmentViews.h"
#import <objc/runtime.h>
#include "Caret.h"
#include "Document.h"
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
  printf("ios_table_cell_break row_broken=%.1f row_plain=%.1f\n", brokenHeight, plainHeight);
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
    printf("native TextKit checks passed\n");
  }
  return 0;
}
