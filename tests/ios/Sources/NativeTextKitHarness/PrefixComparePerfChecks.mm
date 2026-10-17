// Included by main.mm: independent output/layout checks and isolated tail-update CPU.
#include <time.h>

static NSDictionary *prefixCompareAttributes() {
  return @{NSFontAttributeName: [UIFont systemFontOfSize:16],
           NSForegroundColorAttributeName: UIColor.blackColor};
}

static double prefixCompareThreadMilliseconds() {
  struct timespec value;
  require(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value) == 0, @"could not read prefix comparison CPU time");
  return value.tv_sec * 1000.0 + value.tv_nsec / 1000000.0;
}

static void checkPrefixComparisonParity() {
  NSDictionary *attributes = prefixCompareAttributes();
  NSUInteger outputCases = 0;
  for (NSUInteger length : {NSUInteger(32768), NSUInteger(65536)}) {
    NSMutableString *source = [[@"words " stringByPaddingToLength:length withString:@"words "
                                                   startingAtIndex:0] mutableCopy];
    // One grapheme crosses the existing 512-unit copy boundary.
    [source replaceCharactersInRange:NSMakeRange(510, 2) withString:@"👨‍👩‍👧‍👦é"];
    NSAttributedString *before = [[NSAttributedString alloc] initWithString:source attributes:attributes];
    NSMutableArray<NSAttributedString *> *changes = [NSMutableArray arrayWithObject:before];
    for (NSUInteger location : {NSUInteger(0), NSUInteger(1), NSUInteger(509), NSUInteger(512),
                                NSUInteger(1023), source.length - 2}) {
      NSMutableAttributedString *changed = [before mutableCopy];
      [changed replaceCharactersInRange:NSMakeRange(location, 0) withString:@"日本語😀"];
      [changes addObject:[changed copy]];
    }
    for (NSUInteger location : {NSUInteger(0), NSUInteger(2), NSUInteger(510), NSUInteger(512), source.length - 2}) {
      NSMutableAttributedString *changed = [before mutableCopy];
      [changed addAttribute:NSForegroundColorAttributeName value:UIColor.redColor range:NSMakeRange(location, 1)];
      [changes addObject:[changed copy]];
    }
    [changes addObject:[[NSAttributedString alloc] initWithString:@"" attributes:attributes]];
    [changes addObject:[[NSAttributedString alloc] initWithString:@"short replacement" attributes:attributes]];
    for (NSAttributedString *after in changes) {
      NSTextStorage *updated = [[NSTextStorage alloc] initWithAttributedString:before];
      DSUpdateTextStorage(updated, after);
      NSTextStorage *fresh = [[NSTextStorage alloc] initWithAttributedString:after];
      require([updated isEqualToAttributedString:fresh], @"prefix update differed from cold text and attributes");
      outputCases++;
    }
  }
  NSUInteger unicodeLayouts = 0;
  for (NSArray<NSString *> *pair in @[
    @[@"a👨", @"a👨‍👩‍👧‍👦"], @[@"a👨‍👩‍👧‍👦", @"a👨"],
    @[@"prefix e", @"prefix é"], @[@"prefix é", @"prefix e"],
    @[@"prefix 🇦", @"prefix 🇦🇺"], @[@"prefix 👋", @"prefix 👋🏽"],
    @[@"مرحبا بالعالم", @"مرحبا بالعالم!"], @[@"abc\ndef", @"abc\ndef\nghi"]
  ]) {
    DSTextKitStack *updated = [[DSTextKitStack alloc] initWithLayoutManager:[NSLayoutManager new]];
    DSTextKitStack *fresh = [[DSTextKitStack alloc] initWithLayoutManager:[NSLayoutManager new]];
    updated.textContainer.size = fresh.textContainer.size = CGSizeMake(173, CGFLOAT_MAX);
    [updated.textStorage setAttributedString:[[NSAttributedString alloc] initWithString:pair[0]
                                                                            attributes:attributes]];
    [updated.layoutManager ensureLayoutForTextContainer:updated.textContainer];
    NSAttributedString *after = [[NSAttributedString alloc] initWithString:pair[1] attributes:attributes];
    DSUpdateTextStorage(updated.textStorage, after);
    [fresh.textStorage setAttributedString:after];
    [updated.layoutManager ensureLayoutForTextContainer:updated.textContainer];
    [fresh.layoutManager ensureLayoutForTextContainer:fresh.textContainer];
    require([updated.textStorage isEqualToAttributedString:fresh.textStorage] &&
            updated.layoutManager.numberOfGlyphs == fresh.layoutManager.numberOfGlyphs &&
            CGRectEqualToRect([updated.layoutManager usedRectForTextContainer:updated.textContainer],
                              [fresh.layoutManager usedRectForTextContainer:fresh.textContainer]),
            @"Unicode prefix update differed from cold TextKit glyphs or geometry");
    unicodeLayouts++;
  }
  printf("ios_prefix_comparison_parity output_cases=%lu unicode_layouts=%lu restyles=early,late sizes=32768,65536\n",
         (unsigned long)outputCases, (unsigned long)unicodeLayouts);
}

static double benchmarkPrefixComparison(NSUInteger length, BOOL equal) {
  NSDictionary *attributes = prefixCompareAttributes();
  NSString *source = [@"words " stringByPaddingToLength:length withString:@"words " startingAtIndex:0];
  NSMutableArray<NSAttributedString *> *snapshots = [NSMutableArray new];
  // Build immutable renderings before timing, keeping parsing, styling and
  // initial TextKit construction outside the measured storage update.
  for (NSUInteger step = 0; step < 32; step++) {
    NSString *text = equal ? source : [source stringByPaddingToLength:length + step withString:@"x" startingAtIndex:0];
    [snapshots addObject:[[NSAttributedString alloc] initWithString:text attributes:attributes]];
  }
  NSTextStorage *storage = [[NSTextStorage alloc] initWithAttributedString:snapshots.firstObject];
  for (NSUInteger step = 0; step < 64; step++) DSUpdateTextStorage(storage, snapshots[step % snapshots.count]);
  std::vector<double> samples;
  for (NSUInteger round = 0; round < 6; round++) {
    @autoreleasepool {
      const double start = prefixCompareThreadMilliseconds();
      for (NSUInteger step = 0; step < 512; step++) DSUpdateTextStorage(storage, snapshots[step % snapshots.count]);
      samples.push_back(prefixCompareThreadMilliseconds() - start);
    }
    require([storage isEqualToAttributedString:snapshots.lastObject], @"prefix benchmark ended with stale storage");
  }
  std::sort(samples.begin(), samples.end());
  return (samples[2] + samples[3]) / 2;
}

static void checkPrefixComparisonPerformance() {
  checkPrefixComparisonParity();
  const double stream32 = benchmarkPrefixComparison(32768, NO);
  const double stream64 = benchmarkPrefixComparison(65536, NO);
  const double equal32 = benchmarkPrefixComparison(32768, YES);
  const double equal64 = benchmarkPrefixComparison(65536, YES);
  printf("ios_prefix_comparison updates=512 median_stream_32k_thread_ms=%.3f median_stream_64k_thread_ms=%.3f "
         "median_equal_32k_thread_ms=%.3f median_equal_64k_thread_ms=%.3f\n",
         stream32, stream64, equal32, equal64);
}
