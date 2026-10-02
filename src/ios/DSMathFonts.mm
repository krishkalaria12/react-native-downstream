#import "DSMathFonts.h"

#import "math/MathSpeech.h"

#import <algorithm>
#import <mutex>
#import <string>

using namespace downstream;

namespace {

/// One loaded face, and the graphics font it came from.
///
/// A `CGFontRef` from a data provider has to outlive the `CTFontRef` built on it
/// or the face is read from freed memory, so both are held together. The whole
/// table is one lock rather than a lock per face because a draw pass asks for
/// every face it needs in a row, and a per-face lock would be a lock per glyph.
struct FaceTable {
  std::mutex mutex;
  std::vector<CTFontRef> fonts;
  std::vector<CGFontRef> graphics;
  /// Set once by the math package's installer. A null or missing directory means
  /// no faces, which is a formula that draws nothing rather than a crash.
  std::string directory;
};

/// Deliberately leaked, so a draw that lands during teardown cannot touch a
/// destroyed table.
FaceTable &faces() {
  static auto *instance = new FaceTable();
  return *instance;
}

NSString *fontDirectory() {
  FaceTable &table = faces();
  std::lock_guard<std::mutex> lock(table.mutex);
  if (table.directory.empty()) {
    return nil;
  }
  return [NSString stringWithUTF8String:table.directory.c_str()];
}

} // namespace

/// Tells the font loader where the math package's fonts are. See the header.
void DSMathInstallFontDirectory(const std::string &directory) {
  FaceTable &table = faces();
  std::lock_guard<std::mutex> lock(table.mutex);
  if (table.directory == directory) {
    return;
  }
  [DSMathFonts invalidate];
  table.directory = directory;
}

/// Private, so the header stays the public surface: only `fontFor:` needs to ask
/// whether there is a font directory at all, and answering that separately from
/// `fontFor:`'s own lookup is what keeps a missing package one nil check rather
/// than a nil returned from inside a lock.
@interface DSMathFonts ()
+ (nullable NSString *)resolvedDirectory;
@end

@implementation DSMathFonts

/// Where the package's fonts live, or nil when no math package is installed.
///
/// Two ways to find it, in order: the installer records the path the math pod's
/// `+load` gave it, and failing that the KaTeX fonts are looked for in the
/// framework's own bundle. The second is what makes a font directory vendored
/// inside the app — which is how a static-linked math pod ends up — work without
/// the installer running at all.
+ (nullable NSString *)resolvedDirectory {
  if (NSString *installed = fontDirectory()) {
    return installed;
  }
  for (NSBundle *bundle in [NSBundle allBundles]) {
    NSString *path = [bundle pathForResource:@"fonts" ofType:nil];
    if (path != nil) {
      return path;
    }
  }
  return nil;
}

+ (nullable CTFontRef)fontFor:(MathFont)font size:(CGFloat)size {
  if (font == MathFont::System) {
    return NULL;
  }
  NSString *directory = [DSMathFonts resolvedDirectory];
  if (directory == nil) {
    return nil;
  }
  FaceTable &table = faces();
  std::lock_guard<std::mutex> lock(table.mutex);
  const size_t index = static_cast<size_t>(font);
  if (index < table.fonts.size() && table.fonts[index] != NULL) {
    return CTFontCreateCopyWithAttributes(table.fonts[index], size, NULL, NULL);
  }
  if (table.directory.empty()) {
    return NULL;
  }
  const std::string_view file = mathFontFileName(font);
  if (file.empty()) {
    return NULL;
  }
  const std::string path = table.directory + "/" + std::string(file);
  // Read the bytes and hand them to a data provider directly. The alternative,
  // `CTFontManagerCreateFontDescriptorsFromURL`, registers the font
  // process-wide, which is the collision this class exists to avoid.
  NSData *contents = [NSData dataWithContentsOfFile:[NSString stringWithUTF8String:path.c_str()]];
  if (contents == nil) {
    return NULL;
  }
  CFDataRef cfData = CFDataCreate(kCFAllocatorDefault, (const UInt8 *)contents.bytes, (CFIndex)contents.length);
  if (cfData == NULL) {
    return NULL;
  }
  CGDataProviderRef provider = CGDataProviderCreateWithCFData(cfData);
  CFRelease(cfData);
  if (provider == NULL) {
    return NULL;
  }
  CGFontRef graphics = CGFontCreateWithDataProvider(provider);
  CGDataProviderRelease(provider);
  if (graphics == NULL) {
    return NULL;
  }
  const CTFontRef face =
      CTFontCreateWithGraphicsFont(graphics, size, NULL, NULL);
  if (face == NULL) {
    CGFontRelease(graphics);
    return NULL;
  }
  // The table grows to this index so every earlier slot is initialized, which is
  // what lets the lookup above be a bounds check and a pointer read.
  if (table.fonts.size() <= index) {
    table.fonts.resize(index + 1, NULL);
    table.graphics.resize(index + 1, NULL);
  }
  table.fonts[index] = face;
  table.graphics[index] = graphics;
  return CTFontCreateCopyWithAttributes(face, size, NULL, NULL);
}

+ (CTFontRef)systemFontAtSize:(CGFloat)size {
  return CTFontCreateWithName(CFSTR("System"), size, NULL);
}

+ (UIFont *)systemUIFontAtSize:(CGFloat)size {
  return [UIFont systemFontOfSize:size];
}

+ (void)invalidate {
  FaceTable &table = faces();
  std::lock_guard<std::mutex> lock(table.mutex);
  for (CTFontRef font : table.fonts) {
    if (font != NULL) {
      CFRelease(font);
    }
  }
  for (CGFontRef graphics : table.graphics) {
    if (graphics != NULL) {
      CGFontRelease(graphics);
    }
  }
  table.fonts.clear();
  table.graphics.clear();
}

@end

@implementation DSMathRenderer

+ (UIColor *)colorFor:(uint32_t)argb fallback:(UIColor *)fallback {
  if (argb == kInheritColor) {
    return fallback;
  }
  const CGFloat alpha = ((argb >> 24) & 0xff) / 255.0;
  const CGFloat red = ((argb >> 16) & 0xff) / 255.0;
  const CGFloat green = ((argb >> 8) & 0xff) / 255.0;
  const CGFloat blue = (argb & 0xff) / 255.0;
  return [UIColor colorWithRed:red green:green blue:blue alpha:alpha];
}

+ (CGSize)sizeOfLayout:(const MathLayout &)layout size:(CGFloat)size {
  return CGSizeMake(layout.width * size, (layout.height + layout.depth) * size);
}

+ (void)drawLayout:(const MathLayout &)layout
              size:(CGFloat)size
             color:(UIColor *)color
              scale:(CGFloat)scale
          inContext:(CGContextRef)context {
  if (context == NULL || size <= 0) {
    return;
  }
  // One transform for the whole list, so each item below works in em units
  // against a baseline at the origin. The baseline is at y = height in the
  // layout, and Core Graphics draws downward, so the origin is the formula's
  // baseline rather than its top.
  CGContextSaveGState(context);
  CGContextScaleCTM(context, size * scale, size * scale);
  CGContextTranslateCTM(context, 0, -layout.height);
  CGContextSetShouldAntialias(context, true);

  for (const MathRule &rule : layout.rules) {
    CGContextSaveGState(context);
    CGContextSetFillColorWithColor(context, [self colorFor:rule.color fallback:color].CGColor);
    const CGRect box = CGRectMake(rule.x, rule.y, rule.width, rule.height);
    if (rule.dashed) {
      // A dashed rule is drawn as a run of segments rather than with a dash
      // pattern, because a pattern is in user space and these are em units: a
      // pattern set once at the outer scale would be the wrong length for a
      // formula drawn at a different size.
      const CGFloat dash = std::max<CGFloat>(rule.height, 0.04);
      for (CGFloat x = rule.x; x < rule.x + rule.width; x += dash * 2) {
        const CGFloat end = std::min<CGFloat>(x + dash, rule.x + rule.width);
        CGContextFillRect(context, CGRectMake(x, rule.y, end - x, rule.height));
      }
    } else {
      CGContextFillRect(context, box);
    }
    CGContextRestoreGState(context);
  }

  for (const MathPath &path : layout.paths) {
    if (path.commands.empty()) {
      continue;
    }
    CGContextSaveGState(context);
    CGContextSetFillColorWithColor(context, [self colorFor:path.color fallback:color].CGColor);
    CGContextSetStrokeColorWithColor(context, [self colorFor:path.color fallback:color].CGColor);
    CGContextSetLineWidth(context, path.strokeWidth);
    if (path.fill) {
      CGContextBeginPath(context);
    } else {
      CGContextBeginPath(context);
    }
    for (const PathCommand &command : path.commands) {
      switch (command.op) {
        case PathOp::Move:
          CGContextMoveToPoint(context, path.x + command.x, path.y + command.y);
          break;
        case PathOp::Line:
          CGContextAddLineToPoint(context, path.x + command.x, path.y + command.y);
          break;
        case PathOp::Quad:
          CGContextAddQuadCurveToPoint(context, path.x + command.x, path.y + command.y, path.x + command.x2,
                                       path.y + command.y2);
          break;
        case PathOp::Cubic:
          CGContextAddCurveToPoint(context, path.x + command.x, path.y + command.y, path.x + command.x2,
                                   path.y + command.y2, path.x, path.y);
          break;
        case PathOp::Close:
          CGContextClosePath(context);
          break;
      }
    }
    if (path.fill) {
      CGContextFillPath(context);
    } else {
      CGContextStrokePath(context);
    }
    CGContextRestoreGState(context);
  }

  for (const MathGlyph &glyph : layout.glyphs) {
    if (glyph.codePoint == 0) {
      continue;
    }
    // A size-2 or larger glyph is emitted as a scaled glyph rather than as a
    // path, so a big `\sum` stays a real glyph of Size1 at twice the size
    // instead of an outline this renderer would have to scale by hand.
    CTFontRef face = glyph.font == MathFont::System ? [DSMathFonts systemFontAtSize:glyph.scale]
                                                    : [DSMathFonts fontFor:glyph.font size:glyph.scale];
    if (face == NULL) {
      // A missing face draws nothing rather than a placeholder box: a reader
      // seeing a box where a fraction sign should be is worse off than a
      // formula with one missing symbol.
      continue;
    }
    // A code point past the BMP, such as a math italic letter, is a surrogate
    // pair, and Core Text answers it with the glyph first and a zero second.
    unichar characters[2];
    CFIndex count = 1;
    if (glyph.codePoint > 0xFFFF) {
      const char32_t offset = glyph.codePoint - 0x10000;
      characters[0] = static_cast<unichar>(0xD800 + (offset >> 10));
      characters[1] = static_cast<unichar>(0xDC00 + (offset & 0x3FF));
      count = 2;
    } else {
      characters[0] = static_cast<unichar>(glyph.codePoint);
    }
    CGGlyph glyphs[2] = {0, 0};
    if (!CTFontGetGlyphsForCharacters(face, characters, glyphs, count) || glyphs[0] == 0) {
      CFRelease(face);
      continue;
    }
    CGContextSaveGState(context);
    CGContextSetFillColorWithColor(context, [self colorFor:glyph.color fallback:color].CGColor);
    // The context is y-down and Core Text draws y-up, so the glyph is drawn at
    // the origin of a flipped space placed at its baseline.
    CGContextSetTextMatrix(context, CGAffineTransformIdentity);
    CGContextTranslateCTM(context, glyph.x, glyph.y);
    CGContextScaleCTM(context, 1, -1);
    const CGPoint origin = CGPointZero;
    CTFontDrawGlyphs(face, glyphs, &origin, 1, context);
    CGContextRestoreGState(context);
    CFRelease(face);
  }
  CGContextRestoreGState(context);
}

@end

NSString *DSMathSpeechForTex(const std::string &tex, bool display) {
  const std::string spoken = math::speak(tex, display);
  return [NSString stringWithUTF8String:spoken.c_str()] ?: @"";
}
