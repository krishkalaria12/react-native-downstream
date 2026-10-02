#import "DSMathFonts.h"

#import "math/MathEngine.h"
#import "math/MathSpeech.h"

#import <algorithm>
#import <atomic>
#import <mutex>
#import <string>

using namespace downstream;

namespace {

// A raster must not survive replacement or invalidation of its font faces.
std::atomic<uint64_t> fontGeneration{0};

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

/// Releases every loaded face. The caller holds the table's lock, which is not
/// recursive, so this never takes it.
void releaseFaces(FaceTable &table) {
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

} // namespace

/// Tells the font loader where the math package's fonts are. See the header.
void DSMathInstallFontDirectory(const std::string &directory) {
  FaceTable &table = faces();
  std::lock_guard<std::mutex> lock(table.mutex);
  if (table.directory == directory) {
    return;
  }
  releaseFaces(table);
  table.directory = directory;
  fontGeneration.fetch_add(1, std::memory_order_relaxed);
}

extern "C" void DownstreamInstallMathEngine(const void *parseAndLayout, const void *freeDisplayList,
                                            const void *lastError, const char *fontDirectory) {
  MathEngineFunctions functions{};
  if (!mathEngineFunctionsFromSymbols(parseAndLayout, freeDisplayList, lastError, &functions)) {
    return;
  }
  const std::string directory = fontDirectory != nullptr ? fontDirectory : "";
  MathEngine::install(functions, directory);
  DSMathInstallFontDirectory(directory);
}

@implementation DSMathFonts

/// The face for one KaTeX font, from the directory the math package installed.
/// There is no other place to look: searching the app's bundles for a "fonts"
/// folder would find an icon font's before it found KaTeX's.
+ (nullable CTFontRef)fontFor:(MathFont)font size:(CGFloat)size {
  if (font == MathFont::System) {
    return NULL;
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
  return CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, size, NULL);
}

+ (UIFont *)systemUIFontAtSize:(CGFloat)size {
  return [UIFont systemFontOfSize:size];
}

+ (void)invalidate {
  FaceTable &table = faces();
  std::lock_guard<std::mutex> lock(table.mutex);
  releaseFaces(table);
  fontGeneration.fetch_add(1, std::memory_order_relaxed);
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
  // One transform for the whole list, so each item below works in em units. The
  // origin is the top-left of the formula's box, which is RaTeX's own origin: a
  // glyph's y is its baseline measured down from the top, and the formula's
  // baseline is at y = height.
  const CGFloat unit = size * scale;
  CGContextSaveGState(context);
  CGContextScaleCTM(context, unit, unit);
  CGContextSetShouldAntialias(context, true);
  // RaTeX's own renderers never draw a rule thinner than half a point, so a
  // fraction bar at a small size stays visible. The floor is in points, and this
  // space is in em.
  const CGFloat minimumRule = 0.5 / unit;

  for (const MathRule &rule : layout.rules) {
    CGContextSetFillColorWithColor(context, [self colorFor:rule.color fallback:color].CGColor);
    CGFloat y = rule.y;
    CGFloat height = rule.height;
    if (height < minimumRule) {
      y -= (minimumRule - height) / 2;
      height = minimumRule;
    }
    if (rule.dashed) {
      // Dashes three thicknesses long, as RaTeX draws them, as segments rather
      // than a dash pattern so their length is in em like everything else here.
      const CGFloat dash = height * 3;
      for (CGFloat x = rule.x; x < rule.x + rule.width; x += dash * 2) {
        const CGFloat end = std::min<CGFloat>(x + dash, rule.x + rule.width);
        CGContextFillRect(context, CGRectMake(x, y, end - x, height));
      }
    } else {
      CGContextFillRect(context, CGRectMake(rule.x, y, rule.width, height));
    }
  }

  for (const MathPath &path : layout.paths) {
    if (path.commands.empty()) {
      continue;
    }
    CGContextSaveGState(context);
    UIColor *ink = [self colorFor:path.color fallback:color];
    CGContextSetFillColorWithColor(context, ink.CGColor);
    CGContextSetStrokeColorWithColor(context, ink.CGColor);
    CGContextSetLineWidth(context, path.strokeWidth > 0 ? path.strokeWidth : minimumRule);
    CGContextTranslateCTM(context, path.x, path.y);
    CGContextBeginPath(context);
    for (const PathCommand &command : path.commands) {
      switch (command.op) {
        case PathOp::Move: CGContextMoveToPoint(context, command.x, command.y); break;
        case PathOp::Line: CGContextAddLineToPoint(context, command.x, command.y); break;
        case PathOp::Quad:
          CGContextAddQuadCurveToPoint(context, command.x1, command.y1, command.x, command.y);
          break;
        case PathOp::Cubic:
          CGContextAddCurveToPoint(context, command.x1, command.y1, command.x2, command.y2, command.x, command.y);
          break;
        case PathOp::Close: CGContextClosePath(context); break;
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
    // A large operator comes as a glyph of a Size font with a scale, so a big
    // `\sum` stays a real glyph rather than an outline drawn by hand.
    CTFontRef face = glyph.font == MathFont::System ? [DSMathFonts systemFontAtSize:glyph.scale]
                                                    : [DSMathFonts fontFor:glyph.font size:glyph.scale];
    if (face == NULL) {
      // A missing face draws nothing rather than a placeholder box: a reader
      // seeing a box where a fraction sign should be is worse off than a
      // formula with one missing symbol.
      continue;
    }
    // A code point past the BMP, such as an emoji or a math italic letter, is a
    // surrogate pair, and Core Text answers it with the glyph first.
    UniChar characters[2];
    CFIndex count = 1;
    if (glyph.codePoint > 0xFFFF) {
      const char32_t offset = glyph.codePoint - 0x10000;
      characters[0] = static_cast<UniChar>(0xD800 + (offset >> 10));
      characters[1] = static_cast<UniChar>(0xDC00 + (offset & 0x3FF));
      count = 2;
    } else {
      characters[0] = static_cast<UniChar>(glyph.codePoint);
    }
    CGGlyph glyphs[2] = {0, 0};
    CTFontRef drawn = face;
    if (!CTFontGetGlyphsForCharacters(face, characters, glyphs, count) || glyphs[0] == 0) {
      // The system face has no emoji or CJK glyph of its own; ask Core Text for
      // the face it would cascade to, which is what a text view draws.
      if (glyph.font != MathFont::System) {
        CFRelease(face);
        continue;
      }
      CFStringRef string = CFStringCreateWithCharacters(kCFAllocatorDefault, characters, count);
      drawn = CTFontCreateForString(face, string, CFRangeMake(0, count));
      CFRelease(string);
      if (drawn == NULL || !CTFontGetGlyphsForCharacters(drawn, characters, glyphs, count) || glyphs[0] == 0) {
        if (drawn != NULL) {
          CFRelease(drawn);
        }
        CFRelease(face);
        continue;
      }
    }
    CGContextSaveGState(context);
    CGContextSetFillColorWithColor(context, [self colorFor:glyph.color fallback:color].CGColor);
    // The context is y-down and Core Text draws y-up, so the glyph is drawn at
    // the origin of a flipped space placed at its baseline.
    CGContextSetTextMatrix(context, CGAffineTransformIdentity);
    CGContextTranslateCTM(context, glyph.x, glyph.y);
    CGContextScaleCTM(context, 1, -1);
    const CGPoint origin = CGPointZero;
    CTFontDrawGlyphs(drawn, glyphs, &origin, 1, context);
    CGContextRestoreGState(context);
    if (drawn != face) {
      CFRelease(drawn);
    }
    CFRelease(face);
  }
  CGContextRestoreGState(context);
}

@end

NSAttributedStringKey const DSMathLiteralAttributeName = @"DSMathLiteral";

@implementation DSMathAttachment {
  MathLayout::Handle _layout;
  CGFloat _fontSize;
  UIColor *_color;
  std::mutex _imageMutex;
  UIImage *_cachedImage;
  CGRect _cachedBounds;
  CGFloat _cachedSize;
  CGFloat _cachedScale;
  UIColor *_cachedColor;
  UITraitCollection *_cachedTraits;
  uint64_t _cachedFontGeneration;
}

- (instancetype)initWithLayout:(MathLayout::Handle)layout fontSize:(CGFloat)fontSize color:(UIColor *)color {
  if (!(self = [super initWithData:nil ofType:nil])) {
    return nil;
  }
  _layout = std::move(layout);
  _fontSize = fontSize;
  _color = color ?: UIColor.labelColor;
  return self;
}

/// The size per em this formula draws at in a line `lineWidth` wide.
- (CGFloat)sizeForLineWidth:(CGFloat)lineWidth {
  const CGFloat natural = _layout ? _layout->width * _fontSize : 0;
  if (!(lineWidth > 0) || natural <= lineWidth || natural <= 0) {
    return _fontSize;
  }
  return _fontSize * std::max<CGFloat>(kMinInlineScale, lineWidth / natural);
}

- (CGRect)attachmentBoundsForTextContainer:(NSTextContainer *)textContainer
                      proposedLineFragment:(CGRect)lineFrag
                             glyphPosition:(CGPoint)position
                            characterIndex:(NSUInteger)charIndex {
  if (!_layout) {
    return CGRectZero;
  }
  const CGFloat size = [self sizeForLineWidth:CGRectGetWidth(lineFrag)];
  // The origin is on the baseline and y grows up, so the box starts at minus the
  // formula's depth.
  return CGRectMake(0, -_layout->depth * size, _layout->width * size, (_layout->height + _layout->depth) * size);
}

- (UIImage *)imageForBounds:(CGRect)imageBounds
              textContainer:(NSTextContainer *)textContainer
             characterIndex:(NSUInteger)charIndex {
  if (!_layout || imageBounds.size.width <= 0 || imageBounds.size.height <= 0) {
    return nil;
  }
  const CGFloat total = _layout->height + _layout->depth;
  const CGFloat size = total > 0 ? imageBounds.size.height / total : _fontSize;
  // Resolved against the appearance being drawn, which UIKit sets as the current
  // trait collection while a text view draws.
  UITraitCollection *traits = UITraitCollection.currentTraitCollection;
  UIColor *color = [_color resolvedColorWithTraitCollection:traits];
  const CGFloat displayScale = traits.displayScale > 0 ? traits.displayScale : UIScreen.mainScreen.scale;
  const uint64_t generation = fontGeneration.load(std::memory_order_relaxed);
  // TextKit asks for the same attachment during selection, redraws and scrolling.
  // Keep only its most recent raster, so an appearance or size change replaces it.
  std::lock_guard<std::mutex> lock(_imageMutex);
  if (_cachedImage != nil && CGRectEqualToRect(_cachedBounds, imageBounds) && _cachedSize == size &&
      _cachedScale == displayScale && _cachedFontGeneration == generation &&
      CGColorEqualToColor(_cachedColor.CGColor, color.CGColor) &&
      ![traits hasDifferentColorAppearanceComparedToTraitCollection:_cachedTraits] &&
      traits.displayGamut == _cachedTraits.displayGamut) {
    return _cachedImage;
  }
  const MathLayout::Handle layout = _layout;
  UIGraphicsImageRendererFormat *format = [UIGraphicsImageRendererFormat preferredFormat];
  format.opaque = NO;
  format.scale = displayScale;
  UIGraphicsImageRenderer *renderer = [[UIGraphicsImageRenderer alloc] initWithSize:imageBounds.size format:format];
  UIImage *image = [renderer imageWithActions:^(UIGraphicsImageRendererContext *context) {
    [DSMathRenderer drawLayout:*layout size:size color:color scale:1 inContext:context.CGContext];
  }];
  _cachedImage = image;
  _cachedBounds = imageBounds;
  _cachedSize = size;
  _cachedScale = displayScale;
  _cachedColor = color;
  _cachedTraits = traits;
  _cachedFontGeneration = generation;
  return image;
}

@end

NSString *DSMathSpeechForTex(const std::string &tex, bool display) {
  const std::string spoken = math::speak(tex, display);
  return [NSString stringWithUTF8String:spoken.c_str()] ?: @"";
}
