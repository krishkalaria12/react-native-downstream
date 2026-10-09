// Production renderer checks count native work and compare cold geometry.
static NSUInteger tableCellBuilds;
static NSUInteger tableMeasurements;
static IMP originalTableCellInit;
static IMP originalTableBounds;
static id countedTableCellInit(id object, SEL selector) {
  tableCellBuilds++;
  return reinterpret_cast<id (*)(id, SEL)>(originalTableCellInit)(object, selector);
}
static CGRect countedTableBounds(id object, SEL selector, CGSize size, NSStringDrawingOptions options,
                                  NSStringDrawingContext *context) {
  tableMeasurements++;
  return reinterpret_cast<CGRect (*)(id, SEL, CGSize, NSStringDrawingOptions, NSStringDrawingContext *)>(originalTableBounds)
      (object, selector, size, options, context);
}

static std::shared_ptr<downstream::Document> tableDocument(NSUInteger rows, NSString *wide = nil,
                                                           downstream::CellAlign alignment = downstream::CellAlign::Left) {
  auto document = std::make_shared<downstream::Document>();
  static uint64_t identifier = 970000;
  document->id = identifier++;
  for (NSUInteger index = 0; index < rows; index++) {
    NSString *left = index == 0 ? @"A long header column" : [NSString stringWithFormat:@"item %03lu", (unsigned long)index];
    NSString *right = index == 0 ? @"A second header column" : [NSString stringWithFormat:@"note %03lu", (unsigned long)index];
    if (index == rows - 1 && wide != nil) left = wide;
    NSString *source = [NSString stringWithFormat:@"%@\t%@", left, right];
    std::vector<unichar> characters(source.length);
    [source getCharacters:characters.data() range:NSMakeRange(0, source.length)];
    downstream::Block row;
    row.kind = downstream::BlockKind::TableRow;
    row.table = 1;
    row.tableHeader = index == 0;
    row.text.assign(reinterpret_cast<const char16_t *>(characters.data()), characters.size());
    row.cells = {{0, static_cast<uint32_t>(left.length), alignment},
                 {static_cast<uint32_t>(left.length + 1), static_cast<uint32_t>(source.length)}};
    document->blocks.push_back(std::move(row));
  }
  return document;
}

static DSTableSegment *renderTable(const downstream::DocumentHandle &document, CGFloat scale = 1,
                                   downstream::Theme::Handle theme = downstream::Theme::defaultTheme()) {
  return [DSRenderedDocument renderedDocumentFor:document fontScale:scale syntaxHighlighting:NO theme:theme
                                             caret:downstream::PlacedCaret{}].segments.firstObject;
}
static void compareTable(DSTableSegment *cached, const downstream::DocumentHandle &document, CGFloat scale = 1,
                         downstream::Theme::Handle theme = downstream::Theme::defaultTheme()) {
  [DSRenderedDocument invalidateCaches];
  DSTableSegment *fresh = renderTable(document, scale, theme);
  require([cached.columnWidths isEqual:fresh.columnWidths] && [cached.rowHeights isEqual:fresh.rowHeights] &&
          CGSizeEqualToSize(cached.tableSize, fresh.tableSize), @"cached table differed from cold table geometry");
  for (NSUInteger row = 0; row < cached.rows.count; row++) {
    for (NSUInteger column = 0; column < cached.rows[row].count; column++) {
      require([cached.rows[row][column].text isEqualToAttributedString:fresh.rows[row][column].text],
              @"cached cell lost text, alignment or styling");
      require(![cached.rows[row][column].text isKindOfClass:NSMutableAttributedString.class],
              @"published table cell text is mutable");
    }
  }
}

static void checkTableCellReuse() {
  [DSRenderedDocument invalidateCaches];
  Method init = class_getInstanceMethod(DSTableCell.class, @selector(init));
  Method bounds = class_getInstanceMethod(NSAttributedString.class, @selector(boundingRectWithSize:options:context:));
  originalTableCellInit = method_setImplementation(init, (IMP)countedTableCellInit);
  originalTableBounds = method_setImplementation(bounds, (IMP)countedTableBounds);
  auto firstDocument = tableDocument(32);
  DSTableSegment *first = renderTable(firstDocument);
  NSAttributedString *held = [first.rows[1][0].text copy];
  tableCellBuilds = tableMeasurements = 0;
  auto nextDocument = tableDocument(33);
  DSTableSegment *next = renderTable(nextDocument);
  const NSUInteger appendBuilds = tableCellBuilds, appendMeasures = tableMeasurements;
#ifndef BASELINE
  require(appendBuilds == 2 && appendMeasures <= 4, @"table append rebuilt or remeasured unchanged plain cells");
  require(first.rows[1][0] == next.rows[1][0], @"table append did not share an immutable plain cell");
#endif
  require([first.rows[1][0].text isEqualToAttributedString:held], @"table append mutated an old snapshot");
  method_setImplementation(init, originalTableCellInit);
  method_setImplementation(bounds, originalTableBounds);
  compareTable(next, nextDocument);

  // Growing and shrinking columns must use the current snapshot's maxima.
  NSString *wide = [@"wide " stringByPaddingToLength:300 withString:@"wide " startingAtIndex:0];
  auto wideDocument = tableDocument(33, wide);
  DSTableSegment *widened = renderTable(wideDocument);
  auto narrowDocument = tableDocument(33);
  DSTableSegment *narrowed = renderTable(narrowDocument);
  require(widened.columnWidths[0].doubleValue > narrowed.columnWidths[0].doubleValue,
          @"a shrinking table retained an old widest cell");
  compareTable(widened, wideDocument);
  compareTable(narrowed, narrowDocument);
  for (CGFloat scale : {1.2, 0.9}) {
    auto document = tableDocument(4, nil, downstream::CellAlign::Right);
    compareTable(renderTable(document, scale), document, scale);
  }
  const auto theme = downstream::Theme::create({downstream::theme::kSlotTableColor}, {0xff336699}, {}, 970999, 970999);
  auto themedDocument = tableDocument(4, nil, downstream::CellAlign::Center);
  compareTable(renderTable(themedDocument, 1, theme), themedDocument, 1, theme);
  auto styledDocument = tableDocument(4);
  styledDocument->blocks.tail()[1].runs.push_back({0, 4, downstream::InlineStyle::Strong});
  compareTable(renderTable(styledDocument), styledDocument);
  // A break and RTL plain cell retain the same exact text and measurements.
  auto breaksDocument = tableDocument(4, @"مرحبا\nبالعالم");
  compareTable(renderTable(breaksDocument), breaksDocument);
  printf("ios_table_reuse appended_cells=%lu bounding_rect_calls=%lu cold_geometry_parity=1 shrink=1 font=1 theme=1 alignment=1 styled=1 rtl=1\n",
         (unsigned long)appendBuilds, (unsigned long)appendMeasures);
}

static NSUInteger stackHeightCalls;
static IMP originalStackHeight;
static CGFloat countedStackHeight(id object, SEL selector, CGFloat width) {
  stackHeightCalls++;
  return reinterpret_cast<CGFloat (*)(id, SEL, CGFloat)>(originalStackHeight)(object, selector, width);
}
static void checkExactStackWidths() {
  auto document = std::make_shared<downstream::Document>(*makeDocument(@"Exact width wraps words near a boundary."));
  document->blocks.tail()[0].kind = downstream::BlockKind::Paragraph;
  DSRenderedDocument *rendered = [DSRenderedDocument renderedDocumentFor:document fontScale:1 syntaxHighlighting:NO
                                             theme:downstream::Theme::defaultTheme() caret:downstream::PlacedCaret{}];
  const downstream::CodeLayout code;
  const downstream::ImageStateLayout images;
  const downstream::HostLayout hosts;
  const downstream::TableLayout tables;
  const auto size = [&](CGFloat width) {
    return [rendered sizeForWidth:width chrome:0 code:code images:images hosts:hosts tables:tables];
  };
  const CGSize constraint = size(320.2), mounted = size(320);
  NSArray<NSValue *> *constraintFrames = [rendered segmentFramesForWidth:320.2 chrome:0 code:code images:images hosts:hosts tables:tables];
  NSArray<NSValue *> *mountedFrames = [rendered segmentFramesForWidth:320 chrome:0 code:code images:images hosts:hosts tables:tables];
  require(![constraintFrames isKindOfClass:NSMutableArray.class] && ![mountedFrames isKindOfClass:NSMutableArray.class],
          @"published frame map must be immutable");
  Method height = class_getInstanceMethod(DSTextSegment.class, @selector(heightForWidth:));
  originalStackHeight = method_setImplementation(height, (IMP)countedStackHeight);
  stackHeightCalls = 0;
  for (NSUInteger index = 0; index < 20; index++) {
    require(CGSizeEqualToSize(size(320.2), constraint) && CGSizeEqualToSize(size(320), mounted),
            @"alternating exact widths changed geometry");
    require([rendered segmentFramesForWidth:320.2 chrome:0 code:code images:images hosts:hosts tables:tables] == constraintFrames &&
            [rendered segmentFramesForWidth:320 chrome:0 code:code images:images hosts:hosts tables:tables] == mountedFrames,
            @"cached exact widths rebuilt the boxed frame map");
  }
#ifndef BASELINE
  require(stackHeightCalls == 0, @"alternating exact stack widths repeated segment traversal");
#endif
  const NSUInteger alternating = stackHeightCalls;
  (void)size(319.999);
  require(stackHeightCalls > alternating, @"stack cache reused a merely nearby width");
  method_setImplementation(height, originalStackHeight);
  printf("ios_exact_stack_widths widths=320.2,320 alternating_segment_measure_calls=%lu nearby_width_miss=1\n",
         (unsigned long)alternating);
}

static void checkLazyHostedFallback() {
  Method ensure = class_getInstanceMethod(NSLayoutManager.class, @selector(ensureLayoutForTextContainer:));
  originalEnsureLayout = method_setImplementation(ensure, (IMP)countedEnsureLayout);
  layoutCalls = 0;
  DSRenderedDocument *rendered = [DSRenderedDocument renderedDocumentFor:makeDocument(@"first hosted\nlatest fallback")
                                                 fontScale:1 syntaxHighlighting:NO
                                                 theme:downstream::Theme::defaultTheme() caret:downstream::PlacedCaret{}];
#ifndef BASELINE
  require(layoutCalls == 0, @"building code eagerly shaped fallback text");
#endif
  DSCodeSegment *segment = rendered.segments.firstObject;
  DSCodeSegmentView *view = [DSCodeSegmentView new];
  view.hosted = YES;
  [view setSegment:segment];
  view.frame = CGRectMake(0, 0, 320, 120);
  [view layoutIfNeeded];
  DSTextSegmentView *body = [view valueForKey:@"body"];
#ifndef BASELINE
  require(body.textStorage.length == 0 && layoutCalls == 0, @"hosted code mounted or measured its fallback");
#endif
  require(view.accessibilityElements.count == 0, @"hosted code exposed duplicate fallback accessibility");
  DSRenderedDocument *newest = [DSRenderedDocument renderedDocumentFor:makeDocument(@"latest hosted revision\nnew fallback")
                                                fontScale:1 syntaxHighlighting:NO
                                                theme:downstream::Theme::defaultTheme() caret:downstream::PlacedCaret{}];
  [view setSegment:newest.segments.firstObject];
  view.hosted = NO;
  [view layoutIfNeeded];
  require([body.textStorage.string isEqualToString:((DSCodeSegment *)newest.segments.firstObject).text.attributedString.string],
          @"unhosting did not immediately restore the newest fallback text");
  require(view.accessibilityElements.count > 0, @"unhosting lost fallback accessibility");
  method_setImplementation(ensure, originalEnsureLayout);
  printf("ios_hosted_fallback lazy_build_and_mount=1 newest_fallback_restored=1 accessibility=1\n");
}

@interface UIView (LineMarksHarness)
- (void)drawLineMarksInRect:(CGRect)rect;
@end
static NSUInteger markFragmentCalls;
static BOOL countingMarkFragments;
static IMP originalEnumerateFragments;
typedef void (^HarnessFragmentBlock)(CGRect, CGRect, NSTextContainer *, NSRange, BOOL *);
static void countedEnumerateFragments(id object, SEL selector, NSRange range, HarnessFragmentBlock block) {
  reinterpret_cast<void (*)(id, SEL, NSRange, HarnessFragmentBlock)>(originalEnumerateFragments)
      (object, selector, range, ^(CGRect fragment, CGRect used, NSTextContainer *container, NSRange glyphs, BOOL *stop) {
    if (countingMarkFragments) markFragmentCalls++;
    block(fragment, used, container, glyphs, stop);
  });
}
static void checkVisibleLineMarks() {
  NSString *first = [@"wrapped " stringByPaddingToLength:2000 withString:@"wrapped " startingAtIndex:0];
  auto document = std::make_shared<downstream::Document>(*makeDocument([first stringByAppendingString:@"\nunmarked line"]));
  document->blocks.tail()[0].meta.startLine = 10;
  document->blocks.tail()[0].meta.marks = {{10, 10}, {10, 10}};
  const auto theme = downstream::Theme::create({downstream::theme::kSlotCodeBlockLineMarkColor}, {0x8044cc88}, {}, 971002, 971002);
  DSRenderedDocument *rendered = [DSRenderedDocument renderedDocumentFor:document fontScale:1 syntaxHighlighting:NO
                                             theme:theme caret:downstream::PlacedCaret{}];
  DSCodeSegmentView *view = [DSCodeSegmentView new];
  [view setSegment:rendered.segments.firstObject];
  DSTextSegmentView *body = [view valueForKey:@"body"];
  body.frame = CGRectMake(0, 0, 180, 20000);
  body.textContainer.size = CGSizeMake(180, CGFLOAT_MAX);
  [body.layoutManager ensureLayoutForTextContainer:body.textContainer];
  UIView *gutter = findGutter(view);
  gutter.bounds = CGRectMake(0, 0, 25, 90);
  Method enumerate = class_getInstanceMethod(NSLayoutManager.class, @selector(enumerateLineFragmentsForGlyphRange:usingBlock:));
  originalEnumerateFragments = method_setImplementation(enumerate, (IMP)countedEnumerateFragments);
  UIGraphicsImageRendererFormat *format = [UIGraphicsImageRendererFormat defaultFormat];
  format.scale = 1;
  format.opaque = NO;
  UIGraphicsImageRenderer *renderer = [[UIGraphicsImageRenderer alloc] initWithSize:gutter.bounds.size format:format];
  markFragmentCalls = 0;
  UIImage *actual = [renderer imageWithActions:^(UIGraphicsImageRendererContext *context) {
    countingMarkFragments = YES;
    [gutter drawLineMarksInRect:gutter.bounds];
    countingMarkFragments = NO;
  }];
#ifndef BASELINE
  require(markFragmentCalls > 0 && markFragmentCalls < 12,
          @"line marks did not limit fragment visits to the visible wrapped rows");
#endif
  const NSUInteger visited = markFragmentCalls;
  // Reference draw enumerates the entire marked logical line once, independently
  // of the production dirty-rect glyph lookup and duplicate mark matching.
  UIImage *expected = [renderer imageWithActions:^(UIGraphicsImageRendererContext *context) {
    [[((DSCodeSegment *)rendered.segments.firstObject).theme colorForSlot:downstream::theme::kSlotCodeBlockLineMarkColor] setFill];
    const NSRange glyphs = [body.layoutManager glyphRangeForCharacterRange:NSMakeRange(0, first.length)
                                                              actualCharacterRange:NULL];
    [body.layoutManager enumerateLineFragmentsForGlyphRange:glyphs
                                                usingBlock:^(CGRect fragment, CGRect used, NSTextContainer *container,
                                                             NSRange range, BOOL *stop) {
      const CGRect row = CGRectOffset(fragment, 0, body.textContainerInset.top);
      UIRectFill(CGRectMake(0, row.origin.y, CGRectGetWidth(gutter.bounds), row.size.height));
    }];
  }];
  method_setImplementation(enumerate, originalEnumerateFragments);
  NSData *actualPixels = CFBridgingRelease(CGDataProviderCopyData(CGImageGetDataProvider(actual.CGImage)));
  NSData *expectedPixels = CFBridgingRelease(CGDataProviderCopyData(CGImageGetDataProvider(expected.CGImage)));
  require([actualPixels isEqual:expectedPixels], @"visible wrapped line marks differed from one-fill-per-fragment pixels");
  printf("ios_line_marks source_characters=2000 visible_fragment_visits=%lu duplicate_marks_pixel_parity=1\n", (unsigned long)visited);
}

static NSUInteger codeAssignments;
static IMP originalCodeAssignment;
static void countedCodeAssignment(id object, SEL selector, DSCodeSegment *segment) {
  codeAssignments++;
  reinterpret_cast<void (*)(id, SEL, DSCodeSegment *)>(originalCodeAssignment)(object, selector, segment);
}
static void checkUnchangedSegmentAssignments() {
  auto first = std::make_shared<downstream::Document>(*makeDocument(@"settled code"));
  first->blocks.tail()[0].identity = 990990;
  first->settledBlocks = 1;
  downstream::Block tail;
  tail.kind = downstream::BlockKind::Paragraph;
  tail.text = u"tail";
  first->blocks.push_back(tail);
  auto next = std::make_shared<downstream::Document>(*first);
  next->id += 100000;
  next->blocks.tail()[1].text += u" grows";
  const auto render = [](const downstream::DocumentHandle &document) {
    return [DSRenderedDocument renderedDocumentFor:document fontScale:1 syntaxHighlighting:NO
                                             theme:downstream::Theme::defaultTheme() caret:downstream::PlacedCaret{}];
  };
  DSRenderedDocument *oldRendering = render(first), *newRendering = render(next);
  require(oldRendering.segments.firstObject == newRendering.segments.firstObject, @"fixture did not reuse a committed segment");
  DSDocumentView *view = [[DSDocumentView alloc] initWithFrame:CGRectMake(0, 0, 320, 300)];
  view.rendered = oldRendering;
  Method assign = class_getInstanceMethod(DSCodeSegmentView.class, @selector(setSegment:));
  originalCodeAssignment = method_setImplementation(assign, (IMP)countedCodeAssignment);
  codeAssignments = 0;
  view.rendered = newRendering;
  [view layoutIfNeeded];
#ifndef BASELINE
  require(codeAssignments == 0, @"unchanged committed code received a redundant segment assignment");
#endif
  method_setImplementation(assign, originalCodeAssignment);
  // A hosting change still propagates to a skipped immutable segment.
  downstream::HostLayout hosts;
  hosts.entries.push_back({0, 100});
  view.hostLayout = hosts;
  NSArray *views = [view valueForKey:@"segmentViews"];
  require(((DSCodeSegmentView *)views.firstObject).hosted, @"unchanged segment skipped a subsequent host-state change");
  view.hostLayout = downstream::HostLayout{};
  require(!((DSCodeSegmentView *)views.firstObject).hosted, @"unchanged segment skipped fallback restoration");
  printf("ios_unchanged_segments committed_code_assignments=%lu host_state_change=1 fallback=1\n", (unsigned long)codeAssignments);
}

static NSUInteger viewportAssignments;
static NSUInteger viewportSegmentReads;
static id viewportSegments;
static IMP originalViewportSubscript;
static id countedViewportSubscript(id array, SEL selector, NSUInteger index) {
  if (array == viewportSegments) viewportSegmentReads++;
  return reinterpret_cast<id (*)(id, SEL, NSUInteger)>(originalViewportSubscript)(array, selector, index);
}
static IMP originalViewportAssignment;
static void countedViewportAssignment(id object, SEL selector, DSTextSegment *segment) {
  viewportAssignments++;
  reinterpret_cast<void (*)(id, SEL, DSTextSegment *)>(originalViewportAssignment)(object, selector, segment);
}
@interface DSDocumentView (ViewportChecks)
- (void)refreshSegmentViewport;
@end
@interface DSSegmentViewportClock : NSObject
+ (instancetype)shared;
@property (nonatomic, readonly) NSHashTable *views;
@end
static void refreshViewport(DSDocumentView *view) { [view refreshSegmentViewport]; }
static NSUInteger mountedTextViews(DSDocumentView *view) {
  NSUInteger count = 0;
  for (UIView *child in view.subviews) if ([child isKindOfClass:DSTextSegmentView.class]) count++;
  return count;
}
static void checkSegmentViewport() {
  NSArray<NSValue *> *overlaps = @[[NSValue valueWithCGRect:CGRectMake(0, 0, 320, 20)],
      [NSValue valueWithCGRect:CGRectMake(0, 10000, 320, 20)], [NSValue valueWithCGRect:CGRectMake(0, 10, 320, 20)]];
  require(!DSSegmentFramesAreOrdered(overlaps), @"negative-margin overlap must reject the binary viewport search");
  auto document = std::make_shared<downstream::Document>();
  document->id = 980001;
  for (NSUInteger index = 0; index < 3200; index++) {
    downstream::Block block;
    block.kind = downstream::BlockKind::Paragraph;
    block.identity = 980001 + index;
    NSString *source = [NSString stringWithFormat:@"paragraph %lu", (unsigned long)index];
    std::vector<unichar> characters(source.length);
    [source getCharacters:characters.data() range:NSMakeRange(0, source.length)];
    block.text.assign(reinterpret_cast<const char16_t *>(characters.data()), characters.size());
    document->blocks.push_back(std::move(block));
  }
  document->settledBlocks = document->blocks.size();
  DSRenderedDocument *rendered = [DSRenderedDocument renderedDocumentFor:document fontScale:1 syntaxHighlighting:NO
      theme:downstream::Theme::defaultTheme() caret:downstream::PlacedCaret{}];
  const CGSize size = [rendered sizeForWidth:320 chrome:0 code:downstream::CodeLayout{} images:downstream::ImageStateLayout{}
      hosts:downstream::HostLayout{} tables:downstream::TableLayout{}];
  require(rendered.segments.count >= 50, @"viewport fixture must cover many committed text segments");
  UIWindow *window = [[UIWindow alloc] initWithFrame:CGRectMake(0, 0, 320, 180)];
  UIViewController *controller = [UIViewController new];
  window.rootViewController = controller;
  [window makeKeyAndVisible];
  UIScrollView *scroll = [[UIScrollView alloc] initWithFrame:CGRectMake(0, 0, 320, 180)];
  scroll.contentSize = size;
  [controller.view addSubview:scroll];
  DSDocumentView *view = [[DSDocumentView alloc] initWithFrame:CGRectMake(0, 0, 320, size.height)];
  view.selectable = NO;
  [scroll addSubview:view];
  Method assignment = class_getInstanceMethod(DSTextSegmentView.class, @selector(setSegment:));
  originalViewportAssignment = method_setImplementation(assignment, (IMP)countedViewportAssignment);
  viewportAssignments = 0;
  view.rendered = rendered;
  refreshViewport(view); [view layoutIfNeeded];
  const NSUInteger initialAssignments = viewportAssignments;
  require(mountedTextViews(view) <= 3 && initialAssignments <= 3, @"offscreen text received a TextKit view or assignment");
  viewportSegments = rendered.segments;
  Method subscript = class_getInstanceMethod(object_getClass(viewportSegments), @selector(objectAtIndexedSubscript:));
  originalViewportSubscript = method_setImplementation(subscript, (IMP)countedViewportSubscript);
  NSUInteger maxSegmentReads = 0;
  for (CGFloat offset : {CGFloat(4000), CGFloat(9000), std::max(CGFloat(0), size.height - 180), CGFloat(0)}) {
    viewportSegmentReads = 0;
    scroll.contentOffset = CGPointMake(0, offset);
    refreshViewport(view); [view layoutIfNeeded];
    maxSegmentReads = std::max(maxSegmentReads, viewportSegmentReads);
    require(viewportSegmentReads < 40, @"viewport scroll traversed offscreen segment metadata");
    require(mountedTextViews(view) <= 3, @"scroll retained offscreen text views");
    NSArray<NSValue *> *frames = [rendered segmentFramesForWidth:320 chrome:0 code:downstream::CodeLayout{}
        images:downstream::ImageStateLayout{} hosts:downstream::HostLayout{} tables:downstream::TableLayout{}];
    NSArray *slots = [view valueForKey:@"segmentViews"];
    for (NSUInteger index = 0; index < slots.count; index++) {
      UIView *child = slots[index];
      if ([child isKindOfClass:DSTextSegmentView.class]) {
        require(CGRectEqualToRect(child.frame, frames[index].CGRectValue), @"virtualized segment changed cold geometry");
        require([((DSTextSegmentView *)child).textStorage isEqualToAttributedString:
                 ((DSTextSegment *)rendered.segments[index]).attributedString], @"recycled segment displayed stale text");
      }
    }
  }
  method_setImplementation(subscript, originalViewportSubscript);
  viewportSegments = nil;
  auto replacementDocument = std::make_shared<downstream::Document>(*document);
  replacementDocument->id++;
  replacementDocument->blocks.tail()[0].identity += 10000;
  replacementDocument->blocks.tail()[0].text = u"latest replacement paragraph";
  DSRenderedDocument *replacement = [DSRenderedDocument renderedDocumentFor:replacementDocument fontScale:1 syntaxHighlighting:NO
      theme:downstream::Theme::defaultTheme() caret:downstream::PlacedCaret{}];
  view.rendered = replacement;
  refreshViewport(view); [view layoutIfNeeded];
  BOOL latestVisible = NO;
  for (UIView *child in view.subviews) {
    if ([child isKindOfClass:DSTextSegmentView.class] &&
        [((DSTextSegmentView *)child).textStorage.string containsString:@"latest replacement paragraph"]) latestVisible = YES;
  }
  require(latestVisible, @"replacement source did not update a recycled visible text view");
  require(![((DSTextSegment *)rendered.segments.firstObject).attributedString.string containsString:@"latest replacement"],
          @"replacement source mutated retained immutable text");
  const NSUInteger scrollAssignments = viewportAssignments;
  scroll.hidden = YES; refreshViewport(view);
  require(mountedTextViews(view) == 0, @"hidden ancestor retained mounted text views");
  scroll.hidden = NO; refreshViewport(view);
  require(mountedTextViews(view) > 0, @"re-exposed document did not restore text views");
  view.selectable = YES; [view layoutIfNeeded];
  require(mountedTextViews(view) == rendered.segments.count, @"selection must keep every native text view");
  view.selectable = NO; refreshViewport(view); [view layoutIfNeeded];
  require(mountedTextViews(view) <= 3, @"disabling selection did not restore viewport bounds");
  (void)view.accessibilityElements;
  require(mountedTextViews(view) == rendered.segments.count, @"accessibility enumeration omitted offscreen text");
  DSSegmentViewportClock *clock = DSSegmentViewportClock.shared;
  require(![clock.views containsObject:view], @"accessibility-expanded document kept a viewport clock");
  [view removeFromSuperview];
  require(![clock.views containsObject:view], @"detached document retained a viewport clock registration");
  method_setImplementation(assignment, originalViewportAssignment);
  auto codeDocument = std::make_shared<downstream::Document>(); codeDocument->id = 980099;
  for (NSUInteger index = 0; index < 16; index++) {
    downstream::Block code; code.kind = downstream::BlockKind::CodeBlock; code.text = u"plain code";
    codeDocument->blocks.push_back(std::move(code));
  }
  DSDocumentView *codeOnly = [[DSDocumentView alloc] initWithFrame:CGRectMake(0, 0, 320, 5000)];
  codeOnly.selectable = NO; [scroll addSubview:codeOnly];
  codeOnly.rendered = [DSRenderedDocument renderedDocumentFor:codeDocument fontScale:1 syntaxHighlighting:NO
      theme:downstream::Theme::defaultTheme() caret:downstream::PlacedCaret{}];
  require(![clock.views containsObject:codeOnly], @"ineligible document drove a permanent viewport clock");
  [codeOnly removeFromSuperview];
  window.hidden = YES;
  printf("ios_segment_viewport segments=%lu initial_text_assignments=%lu scroll_assignments=%lu max_segment_reads_per_scroll=%lu mounted_text_views_max=3 geometry_parity=1 recycle_text_parity=1 replacement=1 retained_snapshot=1 hidden_reexposed=1 selection=1 accessibility=1 detach_clock=1 ineligible_clock=0\n",
      (unsigned long)rendered.segments.count, (unsigned long)initialAssignments, (unsigned long)scrollAssignments, (unsigned long)maxSegmentReads);
}
