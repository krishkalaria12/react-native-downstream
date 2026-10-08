// Included by main.mm so the checks use the same real renderer and assertion helpers.
static NSTextStorage *observedTailStorage;
static NSUInteger tailEditedCharacters;
static NSUInteger tailEditCalls;
static IMP originalTailProcessEditing;
static void countedTailProcessEditing(id object, SEL selector) {
  NSTextStorage *storage = object;
  if (storage == observedTailStorage && storage.editedMask != 0) {
    tailEditedCharacters += storage.editedRange.length;
    tailEditCalls++;
  }
  reinterpret_cast<void (*)(id, SEL)>(originalTailProcessEditing)(object, selector);
}

static DSTextSegment *tailSegment(NSString *source, NSRange styleRange = NSMakeRange(NSNotFound, 0),
                                  CGFloat fontScale = 1, downstream::PlacedCaret caret = {},
                                  BOOL heading = NO, BOOL quoted = NO) {
  auto document = std::make_shared<downstream::Document>(*makeDocument(source));
  downstream::Block &block = document->blocks.tail()[0];
  block.kind = heading ? downstream::BlockKind::Heading : downstream::BlockKind::Paragraph;
  block.headingLevel = heading ? 2 : 0;
  if (quoted) {
    downstream::Container container;
    container.kind = downstream::ContainerKind::Quote;
    container.id = 1;
    block.containers.push_back(std::move(container));
  }
  DSRenderedDocument *rendered = [DSRenderedDocument renderedDocumentFor:document fontScale:fontScale syntaxHighlighting:NO
                                             theme:downstream::Theme::defaultTheme()
                                             caret:caret];
  DSTextSegment *segment = rendered.segments.firstObject;
  if (styleRange.location != NSNotFound) {
    NSMutableAttributedString *text = [segment.attributedString mutableCopy];
    [text addAttribute:NSForegroundColorAttributeName value:UIColor.redColor range:styleRange];
    // A test snapshot with an early restyle, independent of parser heuristics.
    [segment setValue:[text copy] forKey:@"attributedString"];
  }
  return segment;
}

static void checkTailMetadataEquality() {
#ifndef BASELINE
  const auto check = [](Class type, NSDictionary *values, NSDictionary *changes) {
    id first = [type new];
    [first setValuesForKeysWithDictionary:values];
    id equal = [type new];
    [equal setValuesForKeysWithDictionary:values];
    require([first isEqual:equal] && [first hash] == [equal hash], @"equal text metadata did not compare by value");
    for (NSString *key in changes) {
      id changed = [type new];
      [changed setValuesForKeysWithDictionary:values];
      [changed setValue:changes[key] forKey:key];
      require(![first isEqual:changed], [@"text metadata ignored changed " stringByAppendingString:key]);
    }
  };
  NSDictionary *containerValues = @{
    @"kind": @0, @"leading": @2, @"width": @4, @"continuesPrevious": @NO,
    @"barWidth": @1, @"markerGap": @3, @"marker": [[NSAttributedString alloc] initWithString:@"1."],
    @"barColor": UIColor.redColor
  };
  check(DSContainerInfo.class, containerValues, @{
    @"kind": @1, @"leading": @3, @"width": @5, @"continuesPrevious": @YES,
    @"barWidth": @2, @"markerGap": @4, @"marker": [[NSAttributedString alloc] initWithString:@"2."],
    @"barColor": UIColor.blueColor
  });
  check(DSDecorationMetrics.class, @{@"color": UIColor.redColor, @"height": @2},
        @{@"color": UIColor.blueColor, @"height": @3});
  check(DSBlockInfo.class, @{
    @"index": @1, @"decoration": @0, @"headingLevel": @0, @"spacingBefore": @2,
    @"contentLeading": @4, @"rightToLeft": @NO, @"containers": @[],
    @"rule": NSNull.null, @"accessibilityPrefix": @"checked"
  }, @{
    @"index": @2, @"decoration": @1, @"headingLevel": @1, @"spacingBefore": @3,
    @"contentLeading": @5, @"rightToLeft": @YES, @"containers": @[[DSContainerInfo new]],
    @"rule": [DSDecorationMetrics new], @"accessibilityPrefix": @"unchecked"
  });
  DSContainerInfo *oldContainer = [DSContainerInfo new];
  DSContainerInfo *newContainer = [DSContainerInfo new];
  [oldContainer setValuesForKeysWithDictionary:containerValues];
  [newContainer setValuesForKeysWithDictionary:containerValues];
  DSBlockInfo *oldBlock = [DSBlockInfo new];
  DSBlockInfo *newBlock = [DSBlockInfo new];
  [oldBlock setValue:@[oldContainer] forKey:@"containers"];
  [newBlock setValue:@[newContainer] forKey:@"containers"];
  require([oldBlock isEqual:newBlock], @"independent nested metadata was not value equivalent");
  [newContainer setValue:@9 forKey:@"markerGap"];
  require(![oldBlock isEqual:newBlock], @"nested container change was ignored");
#endif
}

static void checkTailMeasurementWorkspaces() {
  const auto compareFresh = [](DSTextSegment *segment, CGFloat width) {
    DSTextKitStack *fresh = [[DSTextKitStack alloc] initWithLayoutManager:[NSLayoutManager new]];
    fresh.textContainer.size = CGSizeMake(width, CGFLOAT_MAX);
    [fresh.textStorage setAttributedString:segment.attributedString];
    [fresh.layoutManager ensureLayoutForTextContainer:fresh.textContainer];
    const CGRect bounds = [fresh.layoutManager usedRectForTextContainer:fresh.textContainer];
    require(std::abs([segment heightForWidth:width] - CGRectGetMaxY(bounds)) < 0.0001,
            @"scratch tail height differs from fresh TextKit layout");
    require(std::abs([segment usedWidthForWidth:width] - CGRectGetWidth(bounds)) < 0.0001,
            @"scratch tail used width differs from fresh TextKit layout");
  };
  NSMutableArray<DSTextSegment *> *snapshots = [NSMutableArray new];
  NSMutableString *source = [@"words " stringByPaddingToLength:2048 withString:@"words " startingAtIndex:0].mutableCopy;
  for (NSUInteger step = 0; step < 16; step++) {
    [source appendString:@"next words "];
    DSTextSegment *segment = tailSegment(source);
    [snapshots addObject:segment];
    compareFresh(segment, 320);
    compareFresh(segment, 320.2);
    // Interleave a different message on the same measuring thread.
    compareFresh(tailSegment(@"An independent message with another wrap."), 320);
  }
  for (DSTextSegment *segment in snapshots) {
    // Previously unmeasured width forces an older immutable snapshot through
    // scratch storage currently holding a newer/different message.
    compareFresh(segment, 319.5);
  }
  for (NSString *text in @[@"a👨", @"a👨‍👩‍👧‍👦é", @"مرحبا بالعالم هذه فقرة", @"abc\ndef\nghi", @"a", @""]) {
    compareFresh(tailSegment(text), 320);
    compareFresh(tailSegment(text), 320.2);
  }
  compareFresh(tailSegment(source, NSMakeRange(0, 5)), 320);
  const NSRange noRestyle = NSMakeRange(NSNotFound, 0);
  for (CGFloat scale : {1.0, 1.2, 0.9}) {
    compareFresh(tailSegment(source, noRestyle, scale), 320);
    compareFresh(tailSegment(source, noRestyle, scale, {}, YES, YES), 320);
  }
  const downstream::PlacedCaret caret{downstream::CaretKind::Block, downstream::CaretPlace::AfterText};
  compareFresh(tailSegment(source, noRestyle, 1, caret), 320);
  compareFresh(tailSegment([source stringByAppendingString:@" caret moves"], noRestyle, 1, caret), 320);
  for (CGFloat size : {10.0, 18.0, 12.0}) {
    DSTextSegment *attachmentSegment = tailSegment(@"An attachment follows \ufffc");
    NSMutableAttributedString *attributes = [attachmentSegment.attributedString mutableCopy];
    NSTextAttachment *attachment = [NSTextAttachment new];
    attachment.bounds = CGRectMake(0, -2, size, size * 2);
    [attributes addAttribute:NSAttachmentAttributeName value:attachment range:NSMakeRange(attributes.length - 1, 1)];
    [attributes addAttribute:NSFontAttributeName value:[UIFont systemFontOfSize:size]
                      range:NSMakeRange(0, attributes.length)];
    [attachmentSegment setValue:[attributes copy] forKey:@"attributedString"];
    compareFresh(attachmentSegment, 320);
  }
  DSTextSegmentView *decorated = [DSTextSegmentView new];
  [decorated setSegment:tailSegment(@"quoted", noRestyle, 1, {}, NO, YES)];
  DSTextSegment *nextQuote = tailSegment(@"quoted words", noRestyle, 1, {}, NO, YES);
  [decorated setSegment:nextQuote];
  NSRange decoratedRange;
  [decorated.textStorage attribute:DSBlockInfoAttributeName atIndex:0 longestEffectiveRange:&decoratedRange
                          inRange:NSMakeRange(0, decorated.textStorage.length)];
  require(decoratedRange.length == decorated.textStorage.length,
          @"equal old/new container metadata split one block into duplicate decoration ranges");
  compareFresh(tailSegment([@"x " stringByPaddingToLength:65537 withString:@"x " startingAtIndex:0]), 320);
  // Each thread gets independent scratch storage. Prebuild immutable inputs to
  // keep the fixture's document identifier generator on the calling thread.
  NSMutableArray<DSTextSegment *> *concurrent = [NSMutableArray new];
  for (NSUInteger step = 0; step < 32; step++) {
    [concurrent addObject:tailSegment([source stringByAppendingFormat:@"%lu", (unsigned long)step])];
  }
  dispatch_apply(32, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^(size_t index) {
    compareFresh(concurrent[index], 320);
    compareFresh(concurrent[index], 320.2);
  });
  printf("ios_tail_workspace fresh_geometry_snapshots=16 exact_widths=320,320.2 concurrent_snapshots=32 cap_fallback=1 metric_restyles=1 caret=1 attachments=1\n");
}

static void checkGrowingTextTail() {
  checkTailMetadataEquality();
  checkTailMeasurementWorkspaces();
  DSTextSegmentView *view = [DSTextSegmentView new];
  view.frame = CGRectMake(0, 0, 320, 40000);
  NSString *source = [@"words " stringByPaddingToLength:32768 withString:@"words " startingAtIndex:0];
  DSTextSegment *first = tailSegment(source);
  [view setSegment:first];
  NSAttributedString *heldSnapshot = [first.attributedString copy];
  [view.layoutManager ensureLayoutForTextContainer:view.textContainer];
  Method method = class_getInstanceMethod([view.textStorage class], @selector(processEditing));
  originalTailProcessEditing = method_setImplementation(method, (IMP)countedTailProcessEditing);
  observedTailStorage = view.textStorage;
  tailEditedCharacters = 0;
  tailEditCalls = 0;
  const auto start = std::chrono::steady_clock::now();
  NSUInteger fullReplacementCharacters = 0;
  for (NSUInteger step = 0; step < 20; step++) {
    source = [source stringByAppendingString:@"words "];
    DSTextSegment *next = tailSegment(source);
    fullReplacementCharacters += next.attributedString.length;
    view.selectedRange = NSMakeRange(0, 2);
    [view setSegment:next];
    require([view.textStorage isEqualToAttributedString:next.attributedString], @"tail edit changed rendered attributes");
    require(NSEqualRanges(view.selectedRange, NSMakeRange(0, 0)), @"tail edit did not reset selection");
    [view.layoutManager ensureLayoutForTextContainer:view.textContainer];
  }
  const double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  const NSUInteger appendEditedCharacters = tailEditedCharacters;
  printf("ios_mounted_tail updates=20 initial_characters=32768 edited_characters=%lu full_replacement_characters=%lu edit_calls=%lu total_ms=%.3f\n",
         (unsigned long)appendEditedCharacters, (unsigned long)fullReplacementCharacters, (unsigned long)tailEditCalls, elapsed);
#ifndef BASELINE
  require(appendEditedCharacters < 400, @"growing text tail replaced the unchanged prefix");
#endif
  require([first.attributedString isEqualToAttributedString:heldSnapshot], @"mounted tail edit mutated a held snapshot");

  // A completion or theme change can alter attributes near the beginning.
  DSTextSegment *restyled = tailSegment(source, NSMakeRange(2, 4));
  tailEditedCharacters = 0;
  [view setSegment:restyled];
  require([view.textStorage isEqualToAttributedString:restyled.attributedString], @"early restyle was skipped by suffix update");
  require(tailEditedCharacters >= source.length - 2, @"early restyle did not invalidate the changed prefix");

  // Identical output on another immutable snapshot needs no storage edit.
  DSTextSegment *equal = tailSegment(source, NSMakeRange(2, 4));
  tailEditCalls = 0;
  [view setSegment:equal];
#ifndef BASELINE
  require(tailEditCalls == 0, @"identical rendered output edited text storage");
#endif
  // Deletion, an insertion in the middle, a combining mark and joined emoji
  // must produce exactly the same characters/attributes as a fresh mount.
  for (NSString *text in @[@"a👨", @"a👨‍👩‍👧‍👦", @"a👨‍👩‍👧‍👦e", @"a👨‍👩‍👧‍👦e\u0301", @"a👨‍👩‍👧‍👦", @"a middle 👨‍👩‍👧‍👦", @""]) {
    DSTextSegment *next = tailSegment(text);
    [view setSegment:next];
    DSTextSegmentView *fresh = [DSTextSegmentView new];
    fresh.frame = view.frame;
    [fresh setSegment:next];
    // NSTextStorage fixes fonts for emoji; compare the actual mounted output,
    // including those platform changes, rather than its pre-fix input.
    require([view.textStorage isEqualToAttributedString:fresh.textStorage], @"Unicode/deletion tail edit changed the rendered output");
    [view.layoutManager ensureLayoutForTextContainer:view.textContainer];
    [fresh.layoutManager ensureLayoutForTextContainer:fresh.textContainer];
    require(CGRectEqualToRect([view.layoutManager usedRectForTextContainer:view.textContainer],
                             [fresh.layoutManager usedRectForTextContainer:fresh.textContainer]),
            @"tail edit changed TextKit geometry compared to a fresh mount");
  }
  observedTailStorage = nil;
  method_setImplementation(method, originalTailProcessEditing);
}
