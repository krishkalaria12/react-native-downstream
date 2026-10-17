// Included by main.mm: exercise production cache lookups with exact style keys.
#include <time.h>

static NSUInteger styleKeyJSONCalls;
static IMP originalStyleKeyJSON;
static NSData *countedStyleKeyJSON(id object, SEL selector, id value, NSJSONWritingOptions options,
                                  NSError **error) {
  styleKeyJSONCalls++;
  return reinterpret_cast<NSData *(*)(id, SEL, id, NSJSONWritingOptions, NSError **)>(originalStyleKeyJSON)
      (object, selector, value, options, error);
}

static DSLinkVariants *styleKeyVariants() {
  DSLinkVariants *variants = [DSLinkVariants new];
  variants.prefixes = @[@"https://example.com/"];
  variants.counts = @[@1];
  variants.colors = @[@(0xff336699LL)];
  variants.underlines = @[@YES];
  return variants;
}

static DSTagStyles *styleKeyTags() {
  DSTagStyles *styles = [DSTagStyles new];
  styles.names = @[@"mark"];
  styles.colors = @[@(0xff993366LL)];
  styles.flags = @[@1];
  return styles;
}

static std::shared_ptr<downstream::Document> styleKeyStreamDocument(NSUInteger step) {
  using Blocks = downstream::SharedList<downstream::Block>;
  static const auto prefix = [] {
    std::vector<downstream::Block> blocks;
    for (NSUInteger index = 0; index < 200; index++) {
      downstream::Block block;
      block.identity = 993000000 + index;
      block.kind = index % 2 == 0 ? downstream::BlockKind::Paragraph : downstream::BlockKind::CodeBlock;
      block.text = index % 2 == 0 ? u"link tag" : u"const value = 1;";
      if (index % 2 == 0) {
        block.runs = {{0, 4, 0, 0, -1}, {5, 8, 0, -1, 0}};
      } else {
        block.language = "text";
      }
      blocks.push_back(std::move(block));
    }
    return Blocks::Prefix::extend(nullptr, std::move(blocks));
  }();
  static uint64_t identifier = 993100000;
  auto document = std::make_shared<downstream::Document>();
  document->id = identifier++;
  document->settledBlocks = 200;
  document->streaming = true;
  downstream::Block tail;
  tail.kind = downstream::BlockKind::Paragraph;
  tail.text = std::u16string(20 + step, u'x');
  document->blocks = Blocks(prefix, {std::move(tail)});
  downstream::Link link;
  link.url = "https://example.com/path";
  document->links.push_back(std::move(link));
  downstream::Tag tag;
  tag.name = "mark";
  document->tags.push_back(std::move(tag));
  return document;
}

static DSRenderedDocument *styleKeyRender(const downstream::DocumentHandle &document, DSLinkVariants *variants,
                                          DSTagStyles *tags,
                                          downstream::Theme::Handle theme = downstream::Theme::defaultTheme()) {
  return [DSRenderedDocument renderedDocumentFor:document fontScale:1 syntaxHighlighting:NO theme:theme
                                          caret:downstream::PlacedCaret{} variants:variants tagStyles:tags];
}

static void checkStyleKeyInvalidation() {
  const auto document = styleKeyStreamDocument(0);
  const auto theme = downstream::Theme::create({downstream::theme::kSlotParagraphColor}, {0xff112233}, {},
                                               993200000, 993200000);
  const auto check = [&](DSLinkVariants *variants, DSTagStyles *tags,
                         downstream::Theme::Handle optionTheme = downstream::Theme::defaultTheme()) {
    DSRenderedDocument *warm = styleKeyRender(document, styleKeyVariants(), styleKeyTags());
    DSRenderedDocument *changed = styleKeyRender(document, variants, tags, optionTheme);
    require(changed != warm && changed.segments.firstObject != warm.segments.firstObject,
            @"changed style inputs reused an old committed segment");
    DSTextSegment *text = changed.segments.firstObject;
    NSAttributedString *held = [text.attributedString copy];
    [DSRenderedDocument invalidateCaches];
    DSRenderedDocument *cold = styleKeyRender(document, variants, tags, optionTheme);
    require([held isEqualToAttributedString:((DSTextSegment *)cold.segments.firstObject).attributedString],
            @"warm style cache differs from cold styled rendering");
  };
  // Every serialized field remains part of the key, even if an unmatched entry
  // happens to leave this document's paint unchanged.
  for (NSString *field in @[@"prefixes", @"counts", @"colors", @"underlines"]) {
    DSLinkVariants *variants = styleKeyVariants();
    NSDictionary *changes = @{@"prefixes": @[@"https://other.example/"], @"counts": @[@0],
                               @"colors": @[@(0xff119944LL)], @"underlines": @[@NO]};
    [variants setValue:changes[field] forKey:field];
    check(variants, styleKeyTags());
  }
  for (NSString *field in @[@"names", @"colors", @"flags"]) {
    DSTagStyles *tags = styleKeyTags();
    NSDictionary *changes = @{@"names": @[@"other"], @"colors": @[@(0xff119944LL)], @"flags": @[@2]};
    [tags setValue:changes[field] forKey:field];
    check(styleKeyVariants(), tags);
  }
  check(styleKeyVariants(), styleKeyTags(), theme);
  check(nil, nil);
  // Empty objects keep the same default key as nil, with no JSON work.
  DSRenderedDocument *defaults = styleKeyRender(document, nil, nil);
  require(styleKeyRender(document, [DSLinkVariants new], [DSTagStyles new]) == defaults,
          @"empty style objects missed the default rendering");
  // A setter replaces copied arrays: a later request must not retain a key
  // calculated for an earlier value of the same style object.
  DSLinkVariants *variants = styleKeyVariants();
  DSTagStyles *tags = styleKeyTags();
  DSRenderedDocument *before = styleKeyRender(document, variants, tags);
  variants.colors = @[@(0xff445566LL)];
  tags.flags = @[@2];
  require(styleKeyRender(document, variants, tags) != before, @"style-object mutation retained a stale key");
}

static double styleKeyThreadMilliseconds() {
  struct timespec value;
  require(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value) == 0, @"could not read thread CPU time");
  return value.tv_sec * 1000.0 + value.tv_nsec / 1000000.0;
}

static double benchmarkStyleKeyStream(DSLinkVariants *variants, DSTagStyles *tags) {
  constexpr NSUInteger rounds = 6;
  constexpr NSUInteger updates = 32;
  std::vector<downstream::DocumentHandle> documents;
  for (NSUInteger index = 0; index < rounds * updates; index++) {
    documents.push_back(styleKeyStreamDocument(index));
  }
  [DSRenderedDocument invalidateCaches];
  // Populate native segment caches before timing newly constructed wrappers.
  DSRenderedDocument *prefixOwner = styleKeyRender(styleKeyStreamDocument(0), variants, tags);
  std::vector<double> samples;
  for (NSUInteger round = 0; round < rounds; round++) {
    @autoreleasepool {
      const double start = styleKeyThreadMilliseconds();
      for (NSUInteger update = 0; update < updates; update++) {
        DSRenderedDocument *rendered = styleKeyRender(documents[round * updates + update], variants, tags);
        require(rendered.segments.count == 201 && rendered.segments.firstObject == prefixOwner.segments.firstObject,
                @"style benchmark rebuilt its hot committed prefix");
      }
      samples.push_back(styleKeyThreadMilliseconds() - start);
    }
  }
  std::sort(samples.begin(), samples.end());
  return (samples[rounds / 2 - 1] + samples[rounds / 2]) / 2;
}

static void checkStyleKeyPreparationWork() {
  [DSRenderedDocument invalidateCaches];
  DSLinkVariants *variants = styleKeyVariants();
  DSTagStyles *tags = styleKeyTags();
  auto first = styleKeyStreamDocument(0);
  DSRenderedDocument *prefix = styleKeyRender(first, variants, tags);
  auto next = styleKeyStreamDocument(1);
  Method json = class_getClassMethod(NSJSONSerialization.class, @selector(dataWithJSONObject:options:error:));
  originalStyleKeyJSON = method_setImplementation(json, (IMP)countedStyleKeyJSON);
  styleKeyJSONCalls = 0;
  DSRenderedDocument *streamed = styleKeyRender(next, variants, tags);
  const NSUInteger streamedCalls = styleKeyJSONCalls;
  styleKeyJSONCalls = 0;
  require(styleKeyRender(next, variants, tags) == streamed, @"styled root cache did not hit");
  const NSUInteger cachedCalls = styleKeyJSONCalls;
  auto finished = std::make_shared<downstream::Document>(*next);
  finished->id += 1000000;
  finished->renderReuseFrom = next->id;
  finished->streaming = false;
  finished->settledBlocks = finished->blocks.size();
  styleKeyJSONCalls = 0;
  DSRenderedDocument *reused = styleKeyRender(finished, variants, tags);
  const NSUInteger reusedCalls = styleKeyJSONCalls;
  styleKeyJSONCalls = 0;
  (void)styleKeyRender(styleKeyStreamDocument(2), [DSLinkVariants new], [DSTagStyles new]);
  const NSUInteger emptyCalls = styleKeyJSONCalls;
  method_setImplementation(json, originalStyleKeyJSON);
  require(prefix.segments.firstObject == streamed.segments.firstObject && reused.segments == streamed.segments,
          @"style key fixture did not reuse its committed segments or finished payload");
  require(emptyCalls == 0, @"empty styles serialized JSON");
#if !defined(BASELINE) && !defined(IOS_ROUND2_BASELINE)
  require(streamedCalls == 2 && cachedCalls == 2 && reusedCalls == 2,
          @"style keys were serialized more than once per request");
#endif
  checkStyleKeyInvalidation();
  const double styledCPU = benchmarkStyleKeyStream(variants, tags);
  const double emptyCPU = benchmarkStyleKeyStream([DSLinkVariants new], [DSTagStyles new]);
  printf("ios_style_key_preparation committed_segments=200 json_stream=%lu json_cached=%lu json_reused=%lu json_empty=%lu "
         "updates=32 median_styled_thread_ms=%.3f median_empty_thread_ms=%.3f exact_style_invalidation=1\n",
         (unsigned long)streamedCalls, (unsigned long)cachedCalls, (unsigned long)reusedCalls,
         (unsigned long)emptyCalls, styledCPU, emptyCPU);
  [DSRenderedDocument invalidateCaches];
}
