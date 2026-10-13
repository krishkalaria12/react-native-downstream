// Uses real cache and view ownership; weak references observe release after pools drain.
@interface DSRenderedDocument (RetirementChecks)
+ (id)cache;
@end

// Foundation's limits are advisory. Exercise an implementation that retains
// every insertion, so index boundedness cannot depend on NSCache eviction.
@interface DSUnboundedTestCache : NSCache
@end
@implementation DSUnboundedTestCache
- (void)setCountLimit:(NSUInteger)countLimit {}
@end

static DSRenderedDocument *retirementRendering(const downstream::DocumentHandle &document, BOOL colors = NO) {
  return [DSRenderedDocument renderedDocumentFor:document fontScale:1 syntaxHighlighting:colors
                                          theme:downstream::Theme::defaultTheme() caret:downstream::PlacedCaret{}];
}

static void checkRenderingRetirement() {
  [DSRenderedDocument invalidateCaches];
  DSDocumentView *view = [[DSDocumentView alloc] initWithFrame:CGRectMake(0, 0, 320, 100)];
  NSHashTable<DSRenderedDocument *> *historical = [NSHashTable weakObjectsHashTable];
  for (NSUInteger index = 0; index < 40; index++) {
    @autoreleasepool {
      auto document = makeDocument([NSString stringWithFormat:@"revision %lu", (unsigned long)index]);
      DSRenderedDocument *plain = retirementRendering(document);
      DSRenderedDocument *colored = retirementRendering(document, YES);
      [historical addObject:plain];
      [historical addObject:colored];
      view.rendered = plain;
      view.rendered = colored;
      require(retirementRendering(document) == plain && retirementRendering(document, YES) == colored,
              @"same-document variant changes retired a reusable rendering");
    }
  }
  @autoreleasepool {
    require(historical.allObjects.count <= 2, @"mounted stream accumulated old cached variants");
  }
  @autoreleasepool { view.rendered = nil; }
  require(historical.allObjects.count == 0, @"retired document variants remain cache-owned");

  const auto oldDocument = makeDocument(@"held by another mounted view");
  DSRenderedDocument *held = retirementRendering(oldDocument);
  DSDocumentView *other = [[DSDocumentView alloc] initWithFrame:CGRectMake(0, 0, 320, 100)];
  other.rendered = held;
  view.rendered = held;
  const auto nextDocument = makeDocument(@"next");
  DSRenderedDocument *next = retirementRendering(nextDocument);
  view.rendered = next;
  require(retirementRendering(nextDocument) == next, @"retiring old ID evicted new preparation");
  require(retirementRendering(oldDocument) != held, @"retired ID remained cached");
  DSCodeSegment *code = held.segments.firstObject;
  require([code.text.attributedString.string isEqualToString:@"held by another mounted view"] &&
          [code.text heightForWidth:320] > 0, @"retirement invalidated another owner's segment");

  // The oversized exception is retired by the same ID, without disturbing small entries.
  __weak DSRenderedDocument *oversized;
  @autoreleasepool {
    auto document = makeDocument([@"x" stringByPaddingToLength:600 * 1024 withString:@"x" startingAtIndex:0]);
    DSRenderedDocument *large = retirementRendering(document);
    oversized = large;
    [DSRenderedDocument retireDocumentId:document->id];
  }
  require(oversized == nil, @"retirement kept the oversized preparation slot");
  require(retirementRendering(nextDocument) == next, @"oversized retirement evicted unrelated document");
  view.rendered = nil;
  other.rendered = nil;

  // Prepared-but-never-mounted revisions must not leave an unbounded key index.
  [DSRenderedDocument invalidateCaches];
  id wrapper = [DSRenderedDocument cache];
  NSCache *originalCache = [wrapper valueForKey:@"cache"];
  [wrapper setValue:[DSUnboundedTestCache new] forKey:@"cache"];
  for (NSUInteger index = 0; index < 1024; index++) {
    @autoreleasepool {
      auto document = makeDocument(@"unmounted preparation");
      DSRenderedDocument *current = retirementRendering(document);
      require(retirementRendering(document) == current, @"index overflow evicted incoming preparation");
    }
  }
  const NSUInteger indexedKeys = [[[DSRenderedDocument cache] valueForKey:@"trackedKeyCount"] unsignedIntegerValue];
  require(indexedKeys <= 256, @"cache key index accumulated evicted preparation history");
  [DSRenderedDocument invalidateCaches];
  require([[[DSRenderedDocument cache] valueForKey:@"trackedKeyCount"] unsignedIntegerValue] == 0,
          @"cache invalidation retained retirement metadata");
  [wrapper setValue:originalCache forKey:@"cache"];
  printf("ios_rendering_retirement historical_variants=80 retained_after_clear=0 held_owner_valid=1 oversized_released=1 indexed_keys=%lu\n",
         (unsigned long)indexedKeys);
}
