#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#import <objc/runtime.h>
#import "DSMathFonts.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace downstream;

static void require(BOOL condition, NSString *message) {
  if (!condition) { fprintf(stderr, "FAIL: %s\n", message.UTF8String); exit(1); }
}

static std::atomic<NSUInteger> rasterCalls{0};
static IMP originalDraw;
static void countedDraw(id renderer, SEL selector, const MathLayout &layout, CGFloat size,
                        UIColor *color, CGFloat scale, CGContextRef context) {
  rasterCalls++;
  reinterpret_cast<void (*)(id, SEL, const MathLayout &, CGFloat, UIColor *, CGFloat, CGContextRef)>(originalDraw)(
      renderer, selector, layout, size, color, scale, context);
}

static UITraitCollection *traits(UIUserInterfaceStyle style, CGFloat scale,
                                 UIAccessibilityContrast contrast = UIAccessibilityContrastNormal,
                                 UIDisplayGamut gamut = UIDisplayGamutSRGB) {
  return [UITraitCollection traitCollectionWithTraitsFromCollections:@[
    [UITraitCollection traitCollectionWithUserInterfaceStyle:style],
    [UITraitCollection traitCollectionWithDisplayScale:scale],
    [UITraitCollection traitCollectionWithAccessibilityContrast:contrast],
    [UITraitCollection traitCollectionWithDisplayGamut:gamut]
  ]];
}

static UIImage *image(DSMathAttachment *attachment, CGRect bounds, UITraitCollection *appearance) {
  __block UIImage *result;
  [appearance performAsCurrentTraitCollection:^{
    result = [attachment imageForBounds:bounds textContainer:nil characterIndex:0];
  }];
  return result;
}

static std::array<uint8_t, 4> pixel(UIImage *image, CGFloat x, CGFloat y) {
  const size_t width = CGImageGetWidth(image.CGImage);
  const size_t height = CGImageGetHeight(image.CGImage);
  std::vector<uint8_t> pixels(width * height * 4);
  CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
  CGContextRef context = CGBitmapContextCreate(pixels.data(), width, height, 8, width * 4, colorSpace,
                                             static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedLast) |
                                                 kCGBitmapByteOrder32Big);
  CGColorSpaceRelease(colorSpace);
  require(context != NULL, @"could not inspect raster pixels");
  CGContextDrawImage(context, CGRectMake(0, 0, width, height), image.CGImage);
  CGContextRelease(context);
  const size_t offset = (static_cast<size_t>(y * image.scale) * width + static_cast<size_t>(x * image.scale)) * 4;
  return {pixels[offset], pixels[offset + 1], pixels[offset + 2], pixels[offset + 3]};
}

static void checkAppearanceAndBounds() {
  auto layout = std::make_shared<MathLayout>();
  layout->width = 2;
  layout->height = 0.8;
  layout->depth = 0.2;
  layout->rules = {{0.25, 0.25, 0.5, 0.5}, {1.25, 0.25, 0.5, 0.5, 0xff0000ff}};
  UIColor *color = [UIColor colorWithDynamicProvider:^UIColor *(UITraitCollection *appearance) {
    if (appearance.accessibilityContrast == UIAccessibilityContrastHigh) return UIColor.blueColor;
    return appearance.userInterfaceStyle == UIUserInterfaceStyleDark ? UIColor.greenColor : UIColor.redColor;
  }];
  DSMathAttachment *attachment = [[DSMathAttachment alloc] initWithLayout:layout fontSize:24 color:color];
  CGRect bounds = [attachment attachmentBoundsForTextContainer:nil proposedLineFragment:CGRectMake(0, 0, 300, 80)
                                               glyphPosition:CGPointZero characterIndex:0];
  require(fabs(bounds.size.width - 48) < 0.001 && fabs(bounds.size.height - 24) < 0.001,
          @"natural math attachment dimensions changed");
  require(fabs(bounds.origin.y + 4.8) < 0.001, @"formula depth no longer aligns its baseline");
  CGRect narrow = [attachment attachmentBoundsForTextContainer:nil proposedLineFragment:CGRectMake(0, 0, 10, 80)
                                               glyphPosition:CGPointZero characterIndex:0];
  require(fabs(narrow.size.width - 24) < 0.001, @"inline formula no longer respects minimum half scale");
  auto light = traits(UIUserInterfaceStyleLight, 2);
  auto dark = traits(UIUserInterfaceStyleDark, 2);
  UIImage *first = image(attachment, bounds, light);
  require(first != nil, @"attachment failed to rasterize");
  auto red = pixel(first, 12, 12);
  auto blue = pixel(first, 36, 12);
  auto transparent = pixel(first, 1, 1);
  require(red[0] > 240 && red[1] < 10 && red[3] > 240, @"light appearance did not resolve inherited color");
  require(blue[2] > 240 && blue[0] < 10, @"formula-specific color was replaced by inherited color");
  require(transparent[3] == 0, @"attachment raster lost its transparent background");
#ifndef BASELINE
  require(first.scale == 2, @"attachment did not use current trait display scale");
  const NSUInteger warmedCalls = rasterCalls;
  require(image(attachment, bounds, light) == first && rasterCalls == warmedCalls,
          @"identical TextKit requests rasterized the attachment again");
  require(image(attachment, CGRectZero, light) == nil && image(attachment, bounds, light) == first,
          @"empty bounds changed the last valid raster");
#endif
  UIImage *darkImage = image(attachment, bounds, dark);
  auto green = pixel(darkImage, 12, 12);
  require(green[1] > 240 && green[0] < 10, @"dark appearance reused light pixels");
  UIImage *high = image(attachment, bounds, traits(UIUserInterfaceStyleDark, 2, UIAccessibilityContrastHigh));
  require(pixel(high, 12, 12)[2] > 240, @"increased contrast reused the previous resolved color");
#ifndef BASELINE
  require(high != darkImage && darkImage != first, @"appearance change reused stale images");
  UIImage *scaled = image(attachment, bounds, traits(UIUserInterfaceStyleDark, 3, UIAccessibilityContrastHigh));
  require(scaled != high && scaled.scale == 3, @"display scale change reused stale pixels");
  UIImage *resized = image(attachment, narrow, light);
  require(resized != scaled && fabs(resized.size.height - 12) < 0.001,
          @"line width/font size change reused the previous raster");
  UIImage *restored = image(attachment, bounds, light);
  require(restored != first, @"attachment retained more than its latest raster");
  CGRect moved = bounds;
  moved.origin.x = 1;
  require(image(attachment, moved, light) != restored, @"bounds change did not invalidate raster");
  restored = image(attachment, bounds, light);
  [DSMathFonts invalidate];
  require(image(attachment, bounds, light) != restored, @"font invalidation reused stale raster");
  DSMathAttachment *fixed = [[DSMathAttachment alloc] initWithLayout:layout fontSize:24 color:UIColor.redColor];
  UIImage *srgb = image(fixed, bounds, light);
  require(image(fixed, bounds, traits(UIUserInterfaceStyleLight, 2, UIAccessibilityContrastNormal, UIDisplayGamutP3)) != srgb,
          @"display gamut change reused previous raster format");
  UIImage *warm = image(attachment, bounds, light);
  auto *resultCalls = &rasterCalls;
  std::atomic<bool> consistent{true};
  auto *result = &consistent;
  const NSUInteger beforeConcurrent = rasterCalls;
  dispatch_apply(64, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^(size_t index) {
    @autoreleasepool {
      if (image(attachment, bounds, light) != warm) result->store(false);
    }
  });
  require(consistent.load() && resultCalls->load() == beforeConcurrent,
          @"concurrent warmed attachment requests changed images or rasterized again");
#endif
}

static void benchmarkRepeatedDraws() {
  auto layout = std::make_shared<MathLayout>();
  layout->width = 12;
  layout->height = 1.8;
  layout->depth = 0.2;
  for (size_t i = 0; i < 120; ++i) {
    MathGlyph glyph;
    glyph.font = MathFont::System;
    glyph.codePoint = U'a' + i % 26;
    glyph.x = (i % 30) * 0.4;
    glyph.y = 0.45 + (i / 30) * 0.4;
    glyph.scale = 0.4;
    layout->glyphs.push_back(glyph);
  }
  DSMathAttachment *attachment = [[DSMathAttachment alloc] initWithLayout:layout fontSize:24 color:UIColor.labelColor];
  const CGRect bounds = CGRectMake(0, -4.8, 288, 48);
  auto light = traits(UIUserInterfaceStyleLight, UIScreen.mainScreen.scale);
  image(attachment, bounds, light);
  rasterCalls = 0;
  constexpr size_t iterations = 200;
  std::array<double, 7> times;
  for (double &elapsed : times) {
    const auto start = std::chrono::steady_clock::now();
    for (size_t i = 0; i < iterations; ++i) {
      @autoreleasepool { require(image(attachment, bounds, light) != nil, @"benchmark raster disappeared"); }
    }
    elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  }
  std::sort(times.begin(), times.end());
  printf("ios_inline_math repeated_200_requests_median_ms=%.3f raster_calls=%lu glyphs=120 samples=7\n",
         times[times.size() / 2], (unsigned long)rasterCalls.load());
#ifdef BASELINE
  require(rasterCalls == iterations * times.size(), @"baseline did not reproduce repeated rasterization");
#else
  require(rasterCalls == 0, @"warmed inline math repeated Core Graphics rendering");
#endif
}

int main() {
  @autoreleasepool {
    Method draw = class_getClassMethod([DSMathRenderer class], @selector(drawLayout:size:color:scale:inContext:));
    originalDraw = method_setImplementation(draw, (IMP)countedDraw);
    checkAppearanceAndBounds();
    benchmarkRepeatedDraws();
    method_setImplementation(draw, originalDraw);
    printf("native math attachment checks passed\n");
  }
  return 0;
}
