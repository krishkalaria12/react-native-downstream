#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#import "DSRenderedDocument.h"
#include "Document.h"
#include "theme/Theme.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

static void require(BOOL condition, const char *message) {
  if (!condition) {
    fprintf(stderr, "FAIL: %s\n", message);
    exit(1);
  }
}

static downstream::DocumentHandle documentWithRuns(size_t count, bool variedLinks = false) {
  static uint64_t nextId = 28000;
  auto document = std::make_shared<downstream::Document>();
  document->id = nextId++;
  downstream::Block block;
  block.kind = downstream::BlockKind::Heading;
  block.headingLevel = 2;
  const uint32_t styles[] = {
      downstream::InlineStyle::Superscript,
      downstream::InlineStyle::Subscript,
      downstream::InlineStyle::Strong | downstream::InlineStyle::Superscript,
      downstream::InlineStyle::Code | downstream::InlineStyle::Subscript,
      downstream::InlineStyle::Emphasis | downstream::InlineStyle::Strikethrough,
      downstream::InlineStyle::FootnoteRef,
  };
  for (size_t index = 0; index < count; ++index) {
    const uint32_t start = static_cast<uint32_t>(block.text.size());
    block.text += u"x ";
    block.runs.push_back({start, start + 1, styles[index % 6],
                          variedLinks ? static_cast<int32_t>(index % 24) : -1});
  }
  document->blocks.push_back(std::move(block));
  return document;
}

static DSTextSegment *render(const downstream::DocumentHandle &document,
                             const downstream::Theme::Handle &theme, CGFloat scale = 1) {
  DSRenderedDocument *rendered = [DSRenderedDocument renderedDocumentFor:document
                                                              fontScale:scale
                                                     syntaxHighlighting:NO
                                                                  theme:theme
                                                                  caret:downstream::PlacedCaret{}];
  return (DSTextSegment *)rendered.segments.firstObject;
}

static void checkRunAttributes() {
  const auto theme = downstream::Theme::defaultTheme();
  DSTextSegment *segment = render(documentWithRuns(96, true), theme);
  require(segment.attributedString.length == 192, "inline rendering changed the text length");
  for (NSUInteger index = 0; index < 96; ++index) {
    NSDictionary *attributes = [segment.attributedString attributesAtIndex:index * 2 effectiveRange:NULL];
    require([attributes[DSLinkIndexAttributeName] intValue] == index % 24,
            "cached attributes changed a run's link index");
    if (index >= 24) {
      NSDictionary *previous = [segment.attributedString attributesAtIndex:(index - 24) * 2 effectiveRange:NULL];
      require([attributes isEqualToDictionary:previous], "equal styles and links resolved different attributes");
    }
    const CGFloat offset = [attributes[NSBaselineOffsetAttributeName] doubleValue];
    if (index % 6 == 0 || index % 6 == 2 || index % 6 == 5) {
      require(offset > 0, "superscript or footnote lost its raised baseline");
    } else if (index % 6 == 1 || index % 6 == 3) {
      require(offset < 0, "subscript lost its lowered baseline");
    } else {
      require(offset == 0, "baseline leaked between cached styles");
      require(attributes[NSStrikethroughStyleAttributeName] != nil, "strike style was lost");
    }
    require([segment.attributedString attribute:DSLinkIndexAttributeName atIndex:index * 2 + 1 effectiveRange:NULL] == nil,
            "cached attributes leaked into unstyled text");
  }
  DSTextSegment *large = render(documentWithRuns(96), theme, 1.5);
  UIFont *smallFont = [segment.attributedString attribute:NSFontAttributeName atIndex:0 effectiveRange:NULL];
  UIFont *largeFont = [large.attributedString attribute:NSFontAttributeName atIndex:0 effectiveRange:NULL];
  require(largeFont.pointSize > smallFont.pointSize, "run cache retained another render's font scale");
  printf("ios_inline_attributes runs=96 links=24 scale_isolated=1\n");
  const auto custom = downstream::ThemeRegistry::intern(
      {downstream::theme::kSlotSuperscriptFontScale, downstream::theme::kSlotSuperscriptBaselineOffsetScale},
      {0.65, 0.5}, {});
  DSTextSegment *themed = render(documentWithRuns(96), custom);
  UIFont *base = [themed.attributedString attribute:NSFontAttributeName atIndex:1 effectiveRange:NULL];
  UIFont *script = [themed.attributedString attribute:NSFontAttributeName atIndex:0 effectiveRange:NULL];
  NSNumber *offset = [themed.attributedString attribute:NSBaselineOffsetAttributeName atIndex:0 effectiveRange:NULL];
  require(std::abs(script.pointSize - base.pointSize * 0.65) < 0.01,
          "run cache retained another theme's script size");
  require(std::abs(offset.doubleValue - base.pointSize * 0.5) < 0.01,
          "run cache retained another theme's script offset");
}

static void benchmarkRuns(size_t count) {
  const auto theme = downstream::Theme::defaultTheme();
  std::vector<double> samples;
  for (NSUInteger sample = 0; sample < 11; ++sample) {
    @autoreleasepool {
      const auto document = documentWithRuns(count);
      const auto start = std::chrono::steady_clock::now();
      DSTextSegment *segment = render(document, theme);
      const auto end = std::chrono::steady_clock::now();
      require(segment.attributedString.length == count * 2, "benchmark lost inline text");
      const double ms = std::chrono::duration<double, std::milli>(end - start).count();
      if (sample != 0) samples.push_back(ms);
    }
  }
  std::sort(samples.begin(), samples.end());
  printf("ios_inline_build runs=%zu median_ms=%.3f min_ms=%.3f max_ms=%.3f\n",
         count, (samples[4] + samples[5]) / 2, samples.front(), samples.back());
}

int main() {
  @autoreleasepool {
    checkRunAttributes();
    if (std::getenv("DOWNSTREAM_SKIP_INLINE_BENCH") == nullptr) {
      benchmarkRuns(32);
      benchmarkRuns(512);
      benchmarkRuns(4096);
    }
    printf("native inline run checks passed\n");
  }
  return 0;
}
