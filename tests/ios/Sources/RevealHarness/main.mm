#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#import <objc/runtime.h>
#import "DSRenderedDocument.h"
#import "DSSegmentViews.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>

static void require(BOOL condition, NSString *message) {
  if (!condition) { fprintf(stderr, "FAIL: %s\n", message.UTF8String); exit(1); }
}

@interface HarnessRevealProvider : NSObject <DSRevealProvider>
@property(nonatomic, copy) NSArray<DSRevealRun *> *runs;
@property(nonatomic) NSUInteger queries;
@property(nonatomic) NSUInteger frontierQueries;
@end
@implementation HarnessRevealProvider
- (NSArray<DSRevealRun *> *)revealRunsForCharacterRange:(NSRange)range
                                            blockOffset:(NSInteger)blockOffset
                                            blockRanges:(NSArray<NSValue *> *)blockRanges
                                                     now:(double)now {
  self.queries++;
  return self.runs;
}
- (NSUInteger)revealFrontierForCharacterRange:(NSRange)range
                                   blockOffset:(NSInteger)blockOffset
                                   blockRanges:(NSArray<NSValue *> *)blockRanges
                                            now:(double)now {
  self.frontierQueries++;
  for (DSRevealRun *run in self.runs) {
    NSRange clipped = NSIntersectionRange(run.range, range);
    if (clipped.length > 0 && run.alpha <= 0) return clipped.location;
  }
  return NSNotFound;
}
- (CGFloat)revealAlphaAtBlock:(NSInteger)block offset:(NSUInteger)offset now:(double)now { return 0; }
@end

static NSUInteger glyphConversions = 0;
static IMP originalClampedGlyphs;
static NSRange countedClampedGlyphs(id manager, SEL selector, NSRange range) {
  glyphConversions++;
  return reinterpret_cast<NSRange (*)(id, SEL, NSRange)>(originalClampedGlyphs)(manager, selector, range);
}

static NSUInteger protocolChecks = 0;
static IMP originalConforms;
static BOOL countedConforms(id view, SEL selector, Protocol *protocol) {
  if (protocol == @protocol(DSRevealInvalidatable)) protocolChecks++;
  return reinterpret_cast<BOOL (*)(id, SEL, Protocol *)>(originalConforms)(view, selector, protocol);
}

static void checkHiddenDraws() {
  NSTextStorage *storage = [[NSTextStorage alloc] initWithString:[@"word " stringByPaddingToLength:8192 withString:@"word " startingAtIndex:0]
                                                     attributes:@{NSFontAttributeName: [UIFont systemFontOfSize:14]}];
  DSLayoutManager *manager = [DSLayoutManager new];
  NSTextContainer *container = [[NSTextContainer alloc] initWithSize:CGSizeMake(320, CGFLOAT_MAX)];
  [manager addTextContainer:container];
  [storage addLayoutManager:manager];
  [manager ensureLayoutForTextContainer:container];
  const NSRange glyphs = [manager glyphRangeForTextContainer:container];
  HarnessRevealProvider *provider = [HarnessRevealProvider new];
  NSMutableArray *runs = [NSMutableArray arrayWithCapacity:storage.length];
  for (NSUInteger offset = 0; offset < storage.length; offset++) {
    [runs addObject:[[DSRevealRun alloc] initWithRange:NSMakeRange(offset, 1) alpha:0 rise:4]];
  }
  provider.runs = runs;
  manager.revealProvider = provider;
  Method conversion = class_getInstanceMethod(DSLayoutManager.class, NSSelectorFromString(@"clampedGlyphsFor:"));
  originalClampedGlyphs = method_setImplementation(conversion, (IMP)countedClampedGlyphs);
  glyphConversions = 0;
  UIGraphicsBeginImageContextWithOptions(CGSizeMake(320, 200), NO, 1);
  const auto start = std::chrono::steady_clock::now();
  for (NSUInteger frame = 0; frame < 5; frame++) {
    [manager drawBackgroundForGlyphRange:glyphs atPoint:CGPointZero];
    [manager drawGlyphsForGlyphRange:glyphs atPoint:CGPointZero];
  }
  const double milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  UIGraphicsEndImageContext();
  method_setImplementation(conversion, originalClampedGlyphs);
  printf("ios_hidden_reveal_draw_ms=%.3f glyph_conversions=%lu units=%lu frames=5\n", milliseconds,
         (unsigned long)glyphConversions, (unsigned long)runs.count);
#ifdef BASELINE
  require(glyphConversions == runs.count * 10, @"baseline hidden glyph conversion count changed");
#else
  require(glyphConversions == 0, @"hidden units still convert or draw glyphs");
#endif
}

static void checkSegmentLookup() {
  auto document = std::make_shared<downstream::Document>();
  document->id = 92341;
  constexpr NSUInteger count = 128;
  for (NSUInteger block = 0; block < count; block++) {
    downstream::Block value;
    value.kind = downstream::BlockKind::CodeBlock;
    value.text = u"code";
    document->blocks.push_back(std::move(value));
  }
  DSRenderedDocument *rendered = [DSRenderedDocument renderedDocumentFor:document fontScale:1 syntaxHighlighting:NO
                                                                 theme:downstream::Theme::defaultTheme() caret:downstream::PlacedCaret{}];
  DSDocumentView *view = [DSDocumentView new];
  view.rendered = rendered;
  Method method = class_getInstanceMethod(DSCodeSegmentView.class, @selector(conformsToProtocol:));
  originalConforms = method_getImplementation(method);
  const BOOL added = class_addMethod(DSCodeSegmentView.class, @selector(conformsToProtocol:), (IMP)countedConforms, method_getTypeEncoding(method));
  require(added, @"cannot instrument segment protocol checks");
  protocolChecks = 0;
  constexpr NSUInteger queries = 20000;
  const auto start = std::chrono::steady_clock::now();
  for (NSUInteger iteration = 0; iteration < queries; iteration++) {
    @autoreleasepool {
      NSArray *matches = [view revealViewsForBlock:count - 1];
      require(matches.count == 1 && matches.firstObject == view.subviews.lastObject, @"last block mapped to wrong segment");
    }
  }
  const double milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  method_setImplementation(class_getInstanceMethod(DSCodeSegmentView.class, @selector(conformsToProtocol:)), originalConforms);
  printf("ios_reveal_segment_lookup_ms=%.3f protocol_checks=%lu segments=%lu queries=%lu\n", milliseconds,
         (unsigned long)protocolChecks, (unsigned long)count, (unsigned long)queries);
#ifdef BASELINE
  require(protocolChecks == count * queries, @"baseline did not scan each segment per lookup");
#else
  require(protocolChecks <= queries, @"block lookup still scans segments");
#endif
  require([view revealViewsForBlock:-1].count == 0, @"negative block mapped to a segment");
  require([view revealViewsForBlock:count].count == 0, @"past-end block mapped to a segment");
}

#ifndef BASELINE
static NSMutableArray<NSValue *> *drawnGlyphs;
static IMP originalDrawGlyphs;
static void countedDrawGlyphs(id manager, SEL selector, NSRange range, CGPoint origin) {
  [drawnGlyphs addObject:[NSValue valueWithRange:range]];
  reinterpret_cast<void (*)(id, SEL, NSRange, CGPoint)>(originalDrawGlyphs)(manager, selector, range, origin);
}

static void checkSettledGapsAndCaret() {
  NSTextStorage *storage = [[NSTextStorage alloc] initWithString:@"abcdefghi"
                                                     attributes:@{NSFontAttributeName: [UIFont systemFontOfSize:14]}];
  DSLayoutManager *manager = [DSLayoutManager new];
  NSTextContainer *container = [[NSTextContainer alloc] initWithSize:CGSizeMake(320, CGFLOAT_MAX)];
  [manager addTextContainer:container];
  [storage addLayoutManager:manager];
  [manager ensureLayoutForTextContainer:container];
  HarnessRevealProvider *provider = [HarnessRevealProvider new];
  provider.runs = @[
    [[DSRevealRun alloc] initWithRange:NSMakeRange(0, 2) alpha:0 rise:0],
    [[DSRevealRun alloc] initWithRange:NSMakeRange(4, 2) alpha:0 rise:0],
  ];
  manager.revealProvider = provider;
  Method method = class_getInstanceMethod(NSLayoutManager.class, @selector(drawGlyphsForGlyphRange:atPoint:));
  originalDrawGlyphs = method_setImplementation(method, (IMP)countedDrawGlyphs);
  drawnGlyphs = [NSMutableArray new];
  UIGraphicsBeginImageContextWithOptions(CGSizeMake(320, 100), NO, 1);
  [manager drawGlyphsForGlyphRange:[manager glyphRangeForTextContainer:container] atPoint:CGPointZero];
  require(drawnGlyphs.count == 2, @"settled gap and suffix were omitted or hidden units were drawn");
  require(NSEqualRanges(drawnGlyphs[0].rangeValue, NSMakeRange(2, 2)), @"interior settled glyphs changed");
  require(NSEqualRanges(drawnGlyphs[1].rangeValue, NSMakeRange(6, 3)), @"settled suffix glyphs changed");
  method_setImplementation(method, originalDrawGlyphs);
  Method backgroundMethod = class_getInstanceMethod(NSLayoutManager.class, @selector(drawBackgroundForGlyphRange:atPoint:));
  originalDrawGlyphs = method_setImplementation(backgroundMethod, (IMP)countedDrawGlyphs);
  [drawnGlyphs removeAllObjects];
  [manager drawBackgroundForGlyphRange:[manager glyphRangeForTextContainer:container] atPoint:CGPointZero];
  require(drawnGlyphs.count == 2 && NSEqualRanges(drawnGlyphs[0].rangeValue, NSMakeRange(2, 2)) &&
          NSEqualRanges(drawnGlyphs[1].rangeValue, NSMakeRange(6, 3)), @"settled backgrounds diverged from glyph gaps and suffix");
  method_setImplementation(backgroundMethod, originalDrawGlyphs);
  NSTextAttachment *attachment = [NSTextAttachment new];
  attachment.bounds = CGRectMake(0, 0, 2, 12);
  NSMutableAttributedString *caret = [[NSAttributedString attributedStringWithAttachment:attachment] mutableCopy];
  [caret addAttributes:@{DSCaretKindAttributeName: @(static_cast<NSInteger>(downstream::CaretKind::Block)), NSForegroundColorAttributeName: UIColor.blackColor}
                 range:NSMakeRange(0, 1)];
  [storage appendAttributedString:caret];
  [manager ensureLayoutForTextContainer:container];
  provider.queries = 0;
  provider.frontierQueries = 0;
  [manager drawCaretAtPoint:CGPointZero];
  require(provider.queries == 0 && provider.frontierQueries == 1, @"caret allocated full draw runs instead of querying its frontier");
  UIGraphicsEndImageContext();
  drawnGlyphs = nil;
}

static NSUInteger invalidationCalls = 0;
static NSRange lastInvalidatedRange;
static IMP originalInvalidate;
static void countedInvalidation(id view, SEL selector, NSRange range, NSInteger blockOffset, double now) {
  invalidationCalls++;
  lastInvalidatedRange = range;
  reinterpret_cast<void (*)(id, SEL, NSRange, NSInteger, double)>(originalInvalidate)(view, selector, range, blockOffset, now);
}

static void checkCoalescedInvalidationAndTables() {
  auto document = std::make_shared<downstream::Document>();
  document->id = 92342;
  downstream::Block code;
  code.kind = downstream::BlockKind::CodeBlock;
  code.text = std::u16string(4096, u'x');
  document->blocks.push_back(code);
  for (NSUInteger row = 0; row < 3; row++) {
    downstream::Block block;
    block.kind = downstream::BlockKind::TableRow;
    block.text = u"cell";
    block.table = 1;
    block.tableHeader = row == 0;
    block.cells.push_back({0, 4, downstream::CellAlign::Default});
    document->blocks.push_back(std::move(block));
  }
  document->blocks.push_back(code);
  DSDocumentView *view = [DSDocumentView new];
  view.rendered = [DSRenderedDocument renderedDocumentFor:document fontScale:1 syntaxHighlighting:NO
                                                   theme:downstream::Theme::defaultTheme() caret:downstream::PlacedCaret{}];
  UIView *table = view.subviews[1];
  for (NSInteger row = 1; row <= 3; row++) {
    require([view revealViewForBlock:row] == table, @"later table row failed to map onto the table grid");
  }
  require([view revealViewForBlock:4] == view.subviews[2], @"table mapped the following code block into its grid");
  std::vector<downstream::RevealUnit> units;
  for (uint32_t offset = 0; offset < code.text.size(); offset++) units.push_back({0, offset, offset + 1, 0});
  Method method = class_getInstanceMethod(DSCodeSegmentView.class,
      @selector(invalidateRevealCharacterRange:blockOffset:now:));
  originalInvalidate = method_setImplementation(method, (IMP)countedInvalidation);
  invalidationCalls = 0;
  [view invalidateRevealUnits:units document:*document now:50];
  require(invalidationCalls == 1 && NSEqualRanges(lastInvalidatedRange, NSMakeRange(0, 4096)),
          @"character reveal repeated invalidation rather than coalescing its block");
  // Stale/oversized unit ranges are bounded by the current block string.
  units = {{0, 4000, 9000, 0}, {999, 0, 1, 0}};
  [view invalidateRevealUnits:units document:*document now:50];
  require(invalidationCalls == 2 && NSEqualRanges(lastInvalidatedRange, NSMakeRange(4000, 96)),
          @"coalesced invalidation escaped current text bounds");
  auto emptyDocument = std::make_shared<downstream::Document>();
  emptyDocument->id = 92343;
  downstream::Block emptyFence;
  emptyFence.kind = downstream::BlockKind::CodeBlock;
  emptyDocument->blocks.push_back(emptyFence);
  view.rendered = [DSRenderedDocument renderedDocumentFor:emptyDocument fontScale:1 syntaxHighlighting:NO
                                                   theme:downstream::Theme::defaultTheme() caret:downstream::PlacedCaret{}];
  units = {{0, 0, 1, 0}};
  [view invalidateRevealUnits:units document:*emptyDocument now:50];
  require(invalidationCalls == 3 && NSEqualRanges(lastInvalidatedRange, NSMakeRange(0, 1)),
          @"empty block placeholder lost its finishing display invalidation");
  method_setImplementation(method, originalInvalidate);
  printf("ios_reveal_block_invalidations=1 units=4096 table_rows=3\n");
}
#endif

int main() {
  @autoreleasepool {
    checkHiddenDraws();
    checkSegmentLookup();
#ifndef BASELINE
    checkSettledGapsAndCaret();
    checkCoalescedInvalidationAndTables();
#endif
    printf("native reveal checks passed\n");
  }
  return 0;
}
