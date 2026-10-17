// Included by RevealHarness/main.mm after its shared require helper.
#include <algorithm>
#include <cmath>
#include <vector>

@interface TableDirtyProvider : NSObject <DSRevealProvider>
@property(nonatomic) NSUInteger queries;
@property(nonatomic) NSUInteger changingRow;
@property(nonatomic) NSInteger expectedBlock;
@property(nonatomic) CGFloat alpha;
@end
@implementation TableDirtyProvider
- (NSArray<DSRevealRun *> *)revealRunsForCharacterRange:(NSRange)range
                                          blockOffset:(NSInteger)blockOffset
                                          blockRanges:(NSArray<NSValue *> *)blockRanges now:(double)now {
  return @[];
}
- (NSUInteger)revealFrontierForCharacterRange:(NSRange)range blockOffset:(NSInteger)blockOffset
                                blockRanges:(NSArray<NSValue *> *)blockRanges now:(double)now {
  return NSNotFound;
}
- (CGFloat)revealAlphaAtBlock:(NSInteger)block offset:(NSUInteger)offset now:(double)now {
  require(block >= self.expectedBlock && block < self.expectedBlock + 512 && offset == 0,
          @"table reveal did not query its row's document block");
  self.queries++;
  return static_cast<NSUInteger>(block - self.expectedBlock) == self.changingRow ? self.alpha : 1;
}
@end

static __weak UIView *tableObservedGrid;
static __weak UIView *tableObservedHeader;
static NSUInteger tableFullInvalidations;
static NSMutableArray<NSValue *> *tableGridDirtyRects;
static NSMutableArray<NSValue *> *tableHeaderDirtyRects;
static IMP tableOriginalFullInvalidation;
static IMP tableOriginalRectInvalidation;
static void tableCountFullInvalidation(id view, SEL selector) {
  if (view == tableObservedGrid || view == tableObservedHeader) tableFullInvalidations++;
  reinterpret_cast<void (*)(id, SEL)>(tableOriginalFullInvalidation)(view, selector);
}
static void tableCountRectInvalidation(id view, SEL selector, CGRect rect) {
  if (view == tableObservedGrid) [tableGridDirtyRects addObject:[NSValue valueWithCGRect:rect]];
  if (view == tableObservedHeader) [tableHeaderDirtyRects addObject:[NSValue valueWithCGRect:rect]];
  reinterpret_cast<void (*)(id, SEL, CGRect)>(tableOriginalRectInvalidation)(view, selector, rect);
}

static DSTableSegment *tableDirtySegment(NSUInteger count, BOOL rightToLeft) {
  auto document = std::make_shared<downstream::Document>();
  static uint64_t identifier = 984000;
  document->id = identifier++;
  for (NSUInteger index = 0; index < count; index++) {
    downstream::Block row;
    row.kind = downstream::BlockKind::TableRow;
    row.table = 1;
    row.tableHeader = index == 0;
    row.direction = rightToLeft ? downstream::BlockDirection::RightToLeft : downstream::BlockDirection::LeftToRight;
    row.text = rightToLeft ? u"\u05e9\u05dc\u05d5\u05dd\t\u05e2\u05d5\u05dc\u05dd" : u"first column\tsecond column";
    const auto tab = row.text.find(u'\t');
    row.cells = {{0, static_cast<uint32_t>(tab), downstream::CellAlign::Default},
                 {static_cast<uint32_t>(tab + 1), static_cast<uint32_t>(row.text.size())}};
    document->blocks.push_back(std::move(row));
  }
  return [DSRenderedDocument renderedDocumentFor:document fontScale:1 syntaxHighlighting:NO
      theme:downstream::Theme::defaultTheme() caret:downstream::PlacedCaret{}].segments.firstObject;
}

// Keep the prior frame, repaint only the invalidated pixels, and compare it with
// a fresh complete draw. This covers row fills, fractional-alpha cells and grids.
static void tableCompareDirtyPixels(UIView *grid, DSTableSegment *segment, TableDirtyProvider *provider,
                                    NSUInteger row, CGFloat scale, BOOL headerOnly) {
  CGFloat rowTop = 0;
  for (NSUInteger index = 0; index < row; index++) rowTop += segment.rowHeights[index].doubleValue;
  const CGFloat height = segment.rowHeights[row].doubleValue;
  const CGRect viewport = CGRectMake(13, headerOnly ? 0 : std::max<CGFloat>(0, rowTop - height),
                                      std::min<CGFloat>(220, segment.tableSize.width - 13),
                                      headerOnly ? height : height * 3);
  const CGRect dirty = CGRectIntersection(CGRectMake(0, rowTop, segment.tableSize.width, height), viewport);
  const size_t width = static_cast<size_t>(ceil(viewport.size.width * scale));
  const size_t pixelsHigh = static_cast<size_t>(ceil(viewport.size.height * scale));
  std::vector<uint8_t> actual(width * pixelsHigh * 4), expected(actual.size());
  CGColorSpaceRef colors = CGColorSpaceCreateDeviceRGB();
  const CGBitmapInfo bitmapInfo = static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedLast) | kCGBitmapByteOrder32Big;
  CGContextRef actualContext = CGBitmapContextCreate(actual.data(), width, pixelsHigh, 8, width * 4, colors,
      bitmapInfo);
  CGContextRef expectedContext = CGBitmapContextCreate(expected.data(), width, pixelsHigh, 8, width * 4, colors,
      bitmapInfo);
  CGColorSpaceRelease(colors);
  require(actualContext != nullptr && expectedContext != nullptr, @"cannot create table pixel contexts");
  auto draw = [&](CGContextRef context, CGRect rect, BOOL fullDraw = NO) {
    CGContextSaveGState(context);
    CGContextScaleCTM(context, scale, -scale);
    CGContextTranslateCTM(context, -viewport.origin.x, -CGRectGetMaxY(viewport));
    CGContextClipToRect(context, rect);
    CGContextSetRGBFillColor(context, 1, 1, 1, 1);
    CGContextFillRect(context, rect);
    UIGraphicsPushContext(context);
    [grid drawRect:fullDraw ? viewport : rect];
    UIGraphicsPopContext();
    CGContextRestoreGState(context);
  };
  provider.changingRow = row;
  provider.alpha = 0.2;
  draw(actualContext, viewport);
  provider.alpha = 0.65;
  draw(actualContext, dirty);
  draw(expectedContext, viewport);
  require(actual == expected, @"partial table reveal redraw differed from a complete draw");
  // Compare dirty-row selection with a complete draw under the identical clip.
  // Fresh buffers matter here: repeatedly clearing a subpixel slice blends its
  // antialiased clip edge with the previous frame and changes those edge pixels
  // even when the renderer issues the same drawing operations.
  const CGFloat hairline = segment.borderWidth > 0 ? segment.borderWidth : 1.0 / grid.traitCollection.displayScale;
  for (CGRect slice : {
           CGRectMake(viewport.origin.x, rowTop + height - hairline * 0.75, viewport.size.width, hairline * 0.5),
           CGRectMake(viewport.origin.x, rowTop + height - hairline, viewport.size.width, hairline),
           CGRectMake(viewport.origin.x, rowTop + height, viewport.size.width, 1)}) {
    const CGRect clipped = CGRectIntersection(slice, viewport);
    if (CGRectIsNull(clipped) || CGRectIsEmpty(clipped)) continue;
    std::fill(actual.begin(), actual.end(), 0);
    std::fill(expected.begin(), expected.end(), 0);
    draw(actualContext, clipped);
    draw(expectedContext, clipped, YES);
    require(actual == expected, [NSString stringWithFormat:
        @"dirty table border slice differed from complete draw: row=%lu scale=%.1f rtl=%d header=%d hairline=%.6f clip=%@",
        (unsigned long)row, scale, segment.rightToLeft, headerOnly, hairline, NSStringFromCGRect(clipped)]);
  }
  CGContextRelease(actualContext);
  CGContextRelease(expectedContext);
}

static void checkTableDirtyReveal() {
  Class gridClass = NSClassFromString(@"DSTableGridView");
  Method full = class_getInstanceMethod(gridClass, @selector(setNeedsDisplay));
  Method partial = class_getInstanceMethod(gridClass, @selector(setNeedsDisplayInRect:));
  tableOriginalFullInvalidation = method_getImplementation(full);
  tableOriginalRectInvalidation = method_getImplementation(partial);
  require(class_addMethod(gridClass, @selector(setNeedsDisplay), (IMP)tableCountFullInvalidation,
                         method_getTypeEncoding(full)), @"cannot instrument table full invalidation");
  require(class_addMethod(gridClass, @selector(setNeedsDisplayInRect:), (IMP)tableCountRectInvalidation,
                         method_getTypeEncoding(partial)), @"cannot instrument table partial invalidation");
  NSUInteger fullQueries = 0, dirtyQueries = 0;
  for (BOOL rightToLeft : {NO, YES}) {
    DSTableSegment *segment = tableDirtySegment(512, rightToLeft);
    require(segment.rightToLeft == rightToLeft, @"table pixel fixture did not exercise its requested direction");
    DSTableSegmentView *view = [[DSTableSegmentView alloc] initWithFrame:CGRectMake(0, 0, 220, 180)];
    [view setSegment:segment];
    view.blockOffset = 37;
    TableDirtyProvider *provider = [TableDirtyProvider new];
    provider.expectedBlock = 37;
    provider.changingRow = 500;
    provider.alpha = 0.5;
    view.revealProvider = provider; // Assigned after the segment, as a recycled view can be.
    [view layoutIfNeeded];
    UIView *grid = [view valueForKey:@"grid"];
    UIView *header = [view valueForKey:@"header"];
    UIScrollView *scroll = [view valueForKey:@"scrollView"];
    tableObservedGrid = grid;
    tableObservedHeader = header;
    tableGridDirtyRects = [NSMutableArray new];
    tableHeaderDirtyRects = [NSMutableArray new];
    CGFloat top = 0;
    for (NSUInteger row = 0; row < 500; row++) top += segment.rowHeights[row].doubleValue;
    const CGFloat height = segment.rowHeights[500].doubleValue;
    scroll.contentOffset = CGPointMake(13, top - segment.rowHeights[0].doubleValue + height / 3);
    [view layoutIfNeeded];
    tableFullInvalidations = 0;
    [tableGridDirtyRects removeAllObjects];
    [tableHeaderDirtyRects removeAllObjects];
    [view invalidateRevealCharacterRange:NSMakeRange(0, 1) blockOffset:537 now:0];
    require(tableFullInvalidations == 0 && tableGridDirtyRects.count == 1 && tableHeaderDirtyRects.count == 0,
            @"body row reveal invalidated the whole table or pinned header");
    const CGRect visible = [grid convertRect:scroll.bounds fromView:scroll];
    const CGRect wanted = CGRectIntersection(CGRectMake(0, top, segment.tableSize.width, height), visible);
    require(CGRectEqualToRect(tableGridDirtyRects.firstObject.CGRectValue, wanted),
            @"table reveal dirty rectangle escaped its visible row");
    [view invalidateRevealCharacterRange:NSMakeRange(0, 1) blockOffset:38 now:0];
    [view invalidateRevealCharacterRange:NSMakeRange(0, 1) blockOffset:36 now:0];
    [view invalidateRevealCharacterRange:NSMakeRange(0, 1) blockOffset:549 now:0];
    require(tableGridDirtyRects.count == 1, @"offscreen or stale row scheduled a table redraw");
    [view invalidateRevealCharacterRange:NSMakeRange(0, 1) blockOffset:37 now:0];
    require(tableHeaderDirtyRects.count == 1 && tableGridDirtyRects.count == 1,
            @"pinned header reveal did not invalidate its separate backing store");
    UIGraphicsBeginImageContextWithOptions(CGSizeMake(220, 180), NO, 1);
    provider.queries = 0;
    [grid drawRect:CGRectMake(0, 0, segment.tableSize.width, segment.tableSize.height)];
    fullQueries = provider.queries;
    provider.queries = 0;
    [grid drawRect:tableGridDirtyRects.firstObject.CGRectValue];
    dirtyQueries = provider.queries;
    UIGraphicsEndImageContext();
    require(fullQueries == 512 && dirtyQueries <= 2, @"dirty table draw still queried unrelated rows");
    for (CGFloat scale : {1.0, 2.0}) {
      for (NSUInteger row : {NSUInteger(1), NSUInteger(250), NSUInteger(511)}) {
        tableCompareDirtyPixels(grid, segment, provider, row, scale, NO);
      }
      tableCompareDirtyPixels(header, segment, provider, 0, scale, YES);
    }
    [segment setValue:@1 forKey:@"borderWidth"];
    tableCompareDirtyPixels(grid, segment, provider, 250, 2, NO);
    tableCompareDirtyPixels(header, segment, provider, 0, 2, YES);
    // A recycled table can move to another document block without a new segment.
    provider.expectedBlock = 71;
    view.blockOffset = 71;
    tableCompareDirtyPixels(header, segment, provider, 0, 1, YES);
    tableObservedGrid = tableObservedHeader = nil;
  }
  method_setImplementation(class_getInstanceMethod(gridClass, @selector(setNeedsDisplay)), tableOriginalFullInvalidation);
  method_setImplementation(class_getInstanceMethod(gridClass, @selector(setNeedsDisplayInRect:)), tableOriginalRectInvalidation);
  printf("ios_table_reveal_row_queries=%lu->%lu rows=512 pixel_cases=22 border_slice_cases=58\n",
         (unsigned long)fullQueries, (unsigned long)dirtyQueries);
}
