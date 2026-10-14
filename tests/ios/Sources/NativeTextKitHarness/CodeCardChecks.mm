// Per-snapshot main-thread cost of a streaming code card, and the card's chrome.
// Included from main.mm.

/// A view tree laid out and drawn as a commit would: every layer whose backing
/// store is dirty runs its `drawRect:` here, at the device's scale.
static double canvasArea = 0;
static NSUInteger canvasDraws = 0;
static IMP originalCanvasDraw;
static void countedCanvasDraw(id view, SEL selector, CGRect rect) {
  canvasArea += rect.size.width * rect.size.height;
  canvasDraws++;
  reinterpret_cast<void (*)(id, SEL, CGRect)>(originalCanvasDraw)(view, selector, rect);
}

static double gutterArea = 0;
static IMP originalGutterDraw;
static void countedGutterDraw(id view, SEL selector, CGRect rect) {
  gutterArea += rect.size.width * rect.size.height;
  reinterpret_cast<void (*)(id, SEL, CGRect)>(originalGutterDraw)(view, selector, rect);
}

static NSMutableDictionary<NSString *, NSNumber *> *displayByClass;
static void displayTree(CALayer *layer) {
  [layer layoutIfNeeded];
  const auto started = std::chrono::steady_clock::now();
  [layer displayIfNeeded];
  if (displayByClass != nil) {
    NSString *name = layer.delegate ? NSStringFromClass([(NSObject *)layer.delegate class]) : NSStringFromClass(layer.class);
    const double elapsed = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - started).count();
    displayByClass[name] = @(displayByClass[name].doubleValue + elapsed);
  }
  for (CALayer *child in layer.sublayers) displayTree(child);
}

static void setScale(UIView *view, CGFloat scale) {
  // Only where it differs: assigning a view's scale redraws it, as a window move
  // would, which is not what a snapshot does on a device.
  if (view.contentScaleFactor != scale) view.contentScaleFactor = scale;
  for (UIView *child in view.subviews) setScale(child, scale);
}

/// Bytes of drawn backing store in the tree: what a commit re-rasterized.
static size_t drawnBytes(CALayer *layer) {
  size_t bytes = 0;
  id contents = layer.contents;
  if (contents != nil && CFGetTypeID((__bridge CFTypeRef)contents) == CGImageGetTypeID()) {
    CGImageRef image = (__bridge CGImageRef)contents;
    bytes += CGImageGetBytesPerRow(image) * CGImageGetHeight(image);
  } else if (contents != nil) {
    bytes += (size_t)(layer.bounds.size.width * layer.contentsScale * layer.bounds.size.height * layer.contentsScale * 4);
  }
  for (CALayer *child in layer.sublayers) bytes += drawnBytes(child);
  return bytes;
}

static void listDrawn(CALayer *layer, int depth) {
  id contents = layer.contents;
  if (contents != nil) {
    printf("drawn %*s%s %s %.0fx%.0f scale=%.0f\n", depth * 2, "", NSStringFromClass(layer.class).UTF8String,
           layer.delegate ? NSStringFromClass([(NSObject *)layer.delegate class]).UTF8String : "-",
           layer.bounds.size.width, layer.bounds.size.height, layer.contentsScale);
  }
  for (CALayer *child in layer.sublayers) listDrawn(child, depth + 1);
}

static NSString *codeLine(NSUInteger line) {
  switch (line % 9) {
    case 0: return [NSString stringWithFormat:@"export async function handler%lu(input: Request) {\n", (unsigned long)line];
    case 1: return @"  const response = await fetch(`https://example.com/api`, { method: \"POST\" })\n";
    case 2: return @"\n";
    case 3: return @"  if (response.status !== 200) return { ok: false } // retry later\n";
    default: return [NSString stringWithFormat:@"    return compute(%lu, \"label\", [1, 2, 3])\n", (unsigned long)line];
  }
}

/**
 * Streams one code block into one card and times what the main thread does for
 * each snapshot: bind, lay out and draw. The measuring thread's work is timed
 * apart. Simulator timings are relative, not a phone's frame time.
 */
static void benchmarkCodeCardStream() {
  NSMutableString *whole = [NSMutableString string];
  for (NSUInteger line = 0; line < 120; line++) [whole appendString:codeLine(line)];
  DSCodeSegmentView *view = [DSCodeSegmentView new];
  downstream::CodeBlockLayout layout{false, getenv("DOWNSTREAM_IOS_CARD_NO_NUMBERS") == nullptr, 1, 0, false};
  std::vector<double> measure;
  std::vector<double> main;
  std::vector<double> bindTimes, layoutTimes, displayTimes;
  size_t lastDrawn = 0;
  NSUInteger end = 0;
  uint32_t seed = 17;
  if (getenv("DOWNSTREAM_IOS_CARD_LIST") != nullptr) {
    Method draw = class_getInstanceMethod(NSClassFromString(@"_UITextViewCanvasView"), @selector(drawRect:));
    if (draw != nil) originalCanvasDraw = method_setImplementation(draw, (IMP)countedCanvasDraw);
    Method gutterDraw = class_getInstanceMethod(NSClassFromString(@"DSCodeGutterView"), @selector(drawRect:));
    if (gutterDraw != nil) originalGutterDraw = method_setImplementation(gutterDraw, (IMP)countedGutterDraw);
  }
  for (int round = 0; round < 2; round++) {
    canvasArea = 0;
    canvasDraws = 0;
    gutterArea = 0;
    displayByClass = round == 1 && getenv("DOWNSTREAM_IOS_CARD_LIST") != nullptr ? [NSMutableDictionary new] : nil;
    measure.clear();
    main.clear();
    bindTimes.clear();
    layoutTimes.clear();
    displayTimes.clear();
    end = 0;
    while (end < whole.length) {
      seed = seed * 1664525 + 1013904223;
      end = MIN(whole.length, end + 4 + (seed >> 24) % 10);
      @autoreleasepool {
        auto started = std::chrono::steady_clock::now();
        DSRenderedDocument *rendered = [DSRenderedDocument renderedDocumentFor:makeDocument([whole substringToIndex:end])
                                                                      fontScale:1
                                                             syntaxHighlighting:NO
                                                                          theme:downstream::Theme::defaultTheme()
                                                                          caret:downstream::PlacedCaret{}];
        DSCodeSegment *segment = rendered.segments.firstObject;
        const CGFloat body = [segment.text heightForWidth:CGFLOAT_MAX];
        measure.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - started).count());
        started = std::chrono::steady_clock::now();
        [view setSegment:segment];
        view.layout = layout;
        const auto bound = std::chrono::steady_clock::now();
        view.frame = CGRectMake(0, 0, 390, 32 + body + 2 * segment.padding);
        setScale(view, 3);
        [view layoutIfNeeded];
        const auto laid = std::chrono::steady_clock::now();
        displayTree(view.layer);
        const auto shown = std::chrono::steady_clock::now();
        main.push_back(std::chrono::duration<double, std::micro>(shown - started).count());
        bindTimes.push_back(std::chrono::duration<double, std::micro>(bound - started).count());
        layoutTimes.push_back(std::chrono::duration<double, std::micro>(laid - bound).count());
        displayTimes.push_back(std::chrono::duration<double, std::micro>(shown - laid).count());
      }
    }
    lastDrawn = drawnBytes(view.layer);
  }
  if (getenv("DOWNSTREAM_IOS_CARD_LIST") != nullptr) {
    listDrawn(view.layer, 0);
    printf("canvas draws=%lu area_per_snapshot_pt2=%.0f gutter_area_per_snapshot_pt2=%.0f\n", (unsigned long)canvasDraws,
           canvasArea / main.size(), gutterArea / main.size());
    for (NSString *name in displayByClass) {
      printf("display_total %s %.1f ms\n", name.UTF8String, displayByClass[name].doubleValue / 1000.0);
    }
    UIView *gutter = findGutter(view);
    printf("gutter frame=%s hidden=%d contents=%d\n", NSStringFromCGRect(gutter.frame).UTF8String, gutter.hidden,
           gutter.layer.contents != nil);
  }
  auto median = [](std::vector<double> values) {
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
  };
  auto quantile = [](std::vector<double> values, double q) {
    std::sort(values.begin(), values.end());
    return values[(size_t)(values.size() * q)];
  };
  std::vector<double> lastQuarter(main.begin() + main.size() * 3 / 4, main.end());
  auto tail = [&](const std::vector<double> &values) {
    return median(std::vector<double>(values.begin() + values.size() * 3 / 4, values.end()));
  };
  printf("ios_code_card_split last_quarter_bind_us=%.1f layout_us=%.1f display_us=%.1f\n", tail(bindTimes),
         tail(layoutTimes), tail(displayTimes));
  printf("ios_code_card_stream snapshots=%zu characters=%lu measure_median_us=%.1f main_median_us=%.1f "
         "main_p95_us=%.1f main_last_quarter_median_us=%.1f drawn_kib=%.0f\n",
         main.size(), (unsigned long)whole.length, median(measure), median(main), quantile(main, 0.95),
         median(lastQuarter), lastDrawn / 1024.0);
}

static downstream::DocumentHandle makeContainedCode(NSString *source, std::vector<downstream::Container> containers) {
  static uint64_t nextId = 972100;
  auto document = std::make_shared<downstream::Document>();
  document->id = nextId++;
  downstream::Block block;
  block.kind = downstream::BlockKind::CodeBlock;
  block.language = "text";
  block.containers = std::move(containers);
  std::vector<unichar> characters(source.length);
  [source getCharacters:characters.data() range:NSMakeRange(0, source.length)];
  block.text = std::u16string(reinterpret_cast<const char16_t *>(characters.data()), characters.size());
  document->blocks.push_back(std::move(block));
  return document;
}

/**
 * Renders cards that use every piece of chrome into PNGs, so a build can be held
 * to another build's pixels: a header row, a themed border and header rule, a
 * list marker and a quote bar, a capped card's bottom fade, and a hosted card.
 */
static void dumpCodeCardScenes(const char *label) {
  NSMutableString *source = [NSMutableString string];
  for (NSUInteger line = 0; line < 14; line++) [source appendString:codeLine(line)];
  const auto themed = downstream::Theme::create(
      {downstream::theme::kSlotCodeBlockBackgroundColor, downstream::theme::kSlotCodeBlockBorderColor,
       downstream::theme::kSlotCodeBlockBorderWidth, downstream::theme::kSlotCodeBlockBorderRadius,
       downstream::theme::kSlotCodeBlockHeaderBackgroundColor, downstream::theme::kSlotCodeBlockHeaderBorderColor},
      {double(0xFFF4F0E8u), double(0xFFCC3355u), 3, 12, double(0xFFE0E8F4u), double(0xFF2266AAu)}, {}, 972001, 972001);
  downstream::Container item;
  item.kind = downstream::ContainerKind::ListItem;
  item.id = 1;
  item.showsMarker = true;
  item.marker = u"12.";
  downstream::Container quote;
  quote.kind = downstream::ContainerKind::Quote;
  quote.id = 2;
  struct Scene {
    const char *name;
    downstream::DocumentHandle document;
    downstream::Theme::Handle theme;
    downstream::CodeBlockLayout layout;
    BOOL hosted;
  };
  std::vector<Scene> scenes = {
      {"plain", makeDocument(source), downstream::Theme::defaultTheme(), {false, true, 1, 0, false}, NO},
      {"themed", makeDocument(source), themed, {false, true, 1, 0, false}, NO},
      {"contained", makeContainedCode(source, {item, quote}), themed, {false, false, 1, 0, false}, NO},
      {"capped", makeDocument(source), downstream::Theme::defaultTheme(), {false, true, 1, 140, false}, NO},
      {"hosted", makeContainedCode(source, {item}), downstream::Theme::defaultTheme(), {false, true, 1, 0, false}, YES},
  };
  for (Scene &scene : scenes) {
    DSRenderedDocument *rendered = [DSRenderedDocument renderedDocumentFor:scene.document
                                                                  fontScale:1
                                                         syntaxHighlighting:NO
                                                                      theme:scene.theme
                                                                      caret:downstream::PlacedCaret{}];
    DSCodeSegment *segment = rendered.segments.firstObject;
    DSCodeSegmentView *view = [DSCodeSegmentView new];
    view.showsHeader = YES;
    [view setSegment:segment];
    view.layout = scene.layout;
    view.streaming = NO;
    view.hosted = scene.hosted;
    const CGFloat body = [segment.text heightForWidth:CGFLOAT_MAX];
    const CGFloat height = scene.layout.maxHeight > 0 ? 32 + scene.layout.maxHeight : 32 + body + 2 * segment.padding;
    view.frame = CGRectMake(0, 0, 390, height);
    setScale(view, 3);
    [view layoutIfNeeded];
    displayTree(view.layer);
    UIGraphicsImageRendererFormat *format = [UIGraphicsImageRendererFormat preferredFormat];
    format.scale = 3;
    format.opaque = YES;
    UIImage *image = [[[UIGraphicsImageRenderer alloc] initWithSize:view.bounds.size format:format]
        imageWithActions:^(UIGraphicsImageRendererContext *context) {
          [UIColor.whiteColor setFill];
          UIRectFill(view.bounds);
          [view.layer renderInContext:context.CGContext];
        }];
    NSString *path = [NSTemporaryDirectory() stringByAppendingPathComponent:
                                                 [NSString stringWithFormat:@"card-%s-%s.png", scene.name, label]];
    [UIImagePNGRepresentation(image) writeToFile:path atomically:YES];
    printf("card_scene %s %s\n", scene.name, path.UTF8String);
  }
}

/// A reply's unsettled tail: paragraphs, one block each, the last still arriving.
static downstream::DocumentHandle makeProse(NSArray<NSString *> *paragraphs) {
  static uint64_t nextId = 973000;
  auto document = std::make_shared<downstream::Document>();
  document->id = nextId++;
  for (NSString *paragraph in paragraphs) {
    downstream::Block block;
    block.kind = downstream::BlockKind::Paragraph;
    std::vector<unichar> characters(paragraph.length);
    [paragraph getCharacters:characters.data() range:NSMakeRange(0, paragraph.length)];
    block.text = std::u16string(reinterpret_cast<const char16_t *>(characters.data()), characters.size());
    document->blocks.push_back(std::move(block));
  }
  return document;
}

/**
 * Streams a multi-paragraph tail into one text view and times what the main
 * thread does per snapshot: bind, lay out and draw.
 */
static void benchmarkProseTailStream() {
  NSMutableArray<NSString *> *paragraphs = [NSMutableArray new];
  for (NSUInteger index = 0; index < 30; index++) {
    [paragraphs addObject:[NSString stringWithFormat:@"Paragraph %lu of a streaming reply wraps across several lines "
                                                     @"of the column, with enough words to need three or four of them.",
                                                     (unsigned long)index]];
  }
  const NSUInteger total = [[paragraphs componentsJoinedByString:@"\n"] length];
  DSTextSegmentView *view = [DSTextSegmentView new];
  view.scrollEnabled = NO;
  std::vector<double> main;
  std::vector<bool> grew;
  CGFloat lastHeight = 0;
  uint32_t seed = 29;
  for (int round = 0; round < 2; round++) {
    main.clear();
    grew.clear();
    NSUInteger received = 0;
    while (received < total) {
      seed = seed * 1664525 + 1013904223;
      received = MIN(total, received + 4 + (seed >> 24) % 10);
      @autoreleasepool {
        NSMutableArray<NSString *> *shown = [NSMutableArray new];
        NSUInteger left = received;
        for (NSString *paragraph in paragraphs) {
          if (left == 0) break;
          const NSUInteger take = MIN(left, paragraph.length);
          [shown addObject:[paragraph substringToIndex:take]];
          left -= MIN(left, paragraph.length + 1);
        }
        DSRenderedDocument *rendered = [DSRenderedDocument renderedDocumentFor:makeProse(shown)
                                                                      fontScale:1
                                                             syntaxHighlighting:NO
                                                                          theme:downstream::Theme::defaultTheme()
                                                                          caret:downstream::PlacedCaret{}];
        DSTextSegment *segment = rendered.segments.firstObject;
        const CGFloat height = [segment heightForWidth:390];
        const auto started = std::chrono::steady_clock::now();
        [view setSegment:segment];
#ifdef BASELINE
        view.frame = CGRectMake(0, 0, 390, height);
#else
        // Framed as the document frames its last, growing text view.
        view.frame = CGRectMake(0, 0, 390, DSSteppedTextHeight(height));
#endif
        setScale(view, 3);
        [view layoutIfNeeded];
        displayTree(view.layer);
        main.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - started).count());
        grew.push_back(height != lastHeight);
        lastHeight = height;
      }
    }
  }
  double grownSum = 0, steadySum = 0;
  size_t grownCount = 0;
  for (size_t index = 0; index < main.size(); index++) {
    if (grew[index]) { grownSum += main[index]; grownCount++; } else steadySum += main[index];
  }
  printf("ios_prose_tail_growth grown=%zu grown_mean_us=%.1f steady=%zu steady_mean_us=%.1f\n", grownCount,
         grownSum / std::max<size_t>(grownCount, 1), main.size() - grownCount,
         steadySum / std::max<size_t>(main.size() - grownCount, 1));
  std::vector<double> sorted = main;
  std::sort(sorted.begin(), sorted.end());
  std::vector<double> lastQuarter(main.begin() + main.size() * 3 / 4, main.end());
  std::sort(lastQuarter.begin(), lastQuarter.end());
  double sum = 0;
  for (double value : main) sum += value;
  printf("ios_prose_tail_stream snapshots=%zu characters=%lu main_median_us=%.1f main_mean_us=%.1f main_p90_us=%.1f "
         "main_p95_us=%.1f main_p99_us=%.1f main_last_quarter_median_us=%.1f\n",
         main.size(), (unsigned long)total, sorted[sorted.size() / 2], sum / main.size(), sorted[sorted.size() * 90 / 100],
         sorted[sorted.size() * 95 / 100], sorted[sorted.size() * 99 / 100], lastQuarter[lastQuarter.size() / 2]);
}

#ifndef BASELINE
/// The card's chrome is layers, not a drawing: no card-sized bitmap, a fill and
/// border straddling the card's edge as a stroked path did, markers on whole
/// pixels, and no fill under a hosted block.
static void checkCodeCardChrome() {
  NSMutableString *source = [NSMutableString string];
  for (NSUInteger line = 0; line < 6; line++) [source appendString:codeLine(line)];
  const auto themed = downstream::Theme::create(
      {downstream::theme::kSlotCodeBlockBorderWidth, downstream::theme::kSlotCodeBlockBorderRadius}, {3, 12}, {},
      972101, 972101);
  downstream::Container item;
  item.kind = downstream::ContainerKind::ListItem;
  item.id = 1;
  item.showsMarker = true;
  item.marker = u"7.";
  for (BOOL hosted : {NO, YES}) {
    DSRenderedDocument *rendered = [DSRenderedDocument renderedDocumentFor:makeContainedCode(source, {item})
                                                                  fontScale:1
                                                         syntaxHighlighting:NO
                                                                      theme:themed
                                                                      caret:downstream::PlacedCaret{}];
    DSCodeSegment *segment = rendered.segments.firstObject;
    // The card draws the list's number and margin; its body's text has neither,
    // or the number appears twice and the code is indented inside the card.
    NSAttributedString *body = segment.text.attributedString;
    DSBlockInfo *info = [body attribute:DSBlockInfoAttributeName atIndex:0 effectiveRange:NULL];
    NSParagraphStyle *paragraph = [body attribute:NSParagraphStyleAttributeName atIndex:0 effectiveRange:NULL];
    require(segment.containers.count == 1 && info.containers.count == 0 && paragraph.headIndent == 0 &&
                paragraph.firstLineHeadIndent == 0,
            @"a contained code body still carries its container's marker or margin");
    DSCodeSegmentView *view = [DSCodeSegmentView new];
    view.showsHeader = YES;
    [view setSegment:segment];
    view.layout = downstream::CodeBlockLayout{false, true, 1, 0, false};
    view.hosted = hosted;
    view.frame = CGRectMake(0, 0, 390, 300);
    setScale(view, 3);
    [view layoutIfNeeded];
    displayTree(view.layer);
    require(view.layer.contents == nil, @"code card drew a bitmap of its own");
    CALayer *card = [view valueForKey:@"_card"];
    require(card.hidden == hosted, @"hosted card kept its fill, or a card lost it");
    if (!hosted) {
      const CGRect expected = CGRectInset(CGRectMake(segment.contentLeading, 32, 390 - segment.contentLeading, 268),
                                          -1.5, -1.5);
      require(CGRectEqualToRect(card.frame, expected) && card.borderWidth == 3 && card.cornerRadius == 13.5,
              @"card border does not straddle the card edge");
    }
    NSArray *markers = [view valueForKey:@"_markers"];
    require(markers.count == 1, @"list marker was not shown beside the card");
    const CGRect frame = ((UIView *)markers.firstObject).frame;
    require(std::fmod(frame.origin.x * 3, 1) == 0 && std::fmod(frame.origin.y * 3, 1) == 0,
            @"marker view is not on whole device pixels");
  }
  printf("ios_code_card_chrome drawn_bitmap=0 straddled_border=1 pixel_aligned_marker=1 hosted_fill=0 body_marker=0\n");
}
#endif
