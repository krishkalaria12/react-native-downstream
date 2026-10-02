#pragma once

#import <CoreGraphics/CoreGraphics.h>
#import <CoreText/CoreText.h>
#import <UIKit/UIKit.h>

#ifdef __cplusplus
#include "math/MathLayout.h"
#endif

NS_ASSUME_NONNULL_BEGIN

#ifdef __cplusplus
/// Tells the font loader where the math package's fonts are. Called once, from
/// the package's `+load`, and only by the package that carries the binary.
///
/// A C function rather than a class method because the caller is a pod's
/// `+load` and a pod is not a caller this header's types can reach.
void DSMathInstallFontDirectory(const std::string &directory);
#endif

/// The KaTeX faces, loaded from the math package's bundle on first use.
///
/// A face is loaded through `CGFontCreateWithDataProvider` and
/// `CTFontCreateWithGraphicsFont` rather than registered with the process. The
/// difference matters: `CTFontManagerRegisterGraphicsFont` puts a font in every
/// `UIFont` lookup in the app, so two copies of KaTeX_Main — this library's and
/// another markdown library's — would collide by name and whichever registered
/// last would win. A private `CTFontRef` has no such global effect, and it is
/// the only reason a second markdown library can draw math on the same screen.
@interface DSMathFonts : NSObject

/// The face for a math font, at `size` points. Nil for an unknown font, and for
/// `MathFont::System`, which is the platform's own face rather than a KaTeX one.
+ (nullable CTFontRef)fontFor:(downstream::MathFont)font size:(CGFloat)size CF_RETURNS_RETAINED;

/// The platform's own face at `size`, which is what a code point KaTeX has no
/// glyph for draws in.
+ (CTFontRef)systemFontAtSize:(CGFloat)size;

/// The font size scaled by a platform font scale, for a caller that has to build
/// a `UIFont` rather than a `CTFont`.
+ (UIFont *)systemUIFontAtSize:(CGFloat)size;

/// Forgets every loaded face. For a test, and for a caller that has released the
/// package's bundle.
+ (void)invalidate;

@end

/// Draws a display list into a Core Graphics context.
///
/// One renderer for both platforms' needs on this side, so the iOS math block and
/// the iOS inline attachment cannot drift: a formula in a sentence and the same
/// formula on its own line are the same call with a different origin.
///
/// Vectors, drawn at the size the caller asks for, so a formula is sharp at any
/// zoom and in either appearance without being laid out again. The layout is in
/// em units and the baseline is at `y = height`, so `size` is the only input that
/// changes what a reader sees.
@interface DSMathRenderer : NSObject

/// Draws `layout` at `size` points with `color` as the color for every item the
/// formula did not color itself.
///
/// `color` is the run's own dynamic color, so dark mode, a theme, and increased
/// contrast all reach a formula without a relayout: the layout carries no theme,
/// and an item that drew black would ignore all three.
+ (void)drawLayout:(const downstream::MathLayout &)layout
              size:(CGFloat)size
             color:(UIColor *)color
              scale:(CGFloat)scale
          inContext:(CGContextRef)context;

/// The size a formula occupies at `size` points, which is what an attachment's
/// bounds and a block's height both read.
+ (CGSize)sizeOfLayout:(const downstream::MathLayout &)layout size:(CGFloat)size;

@end

/// The spoken form of a formula, from the engine's verbalizer.
///
/// A `NSString` rather than a synthesized attribute so a caller can use it for a
/// block's accessibility label and for an inline run's speech attribute without a
/// second entry point.
NSString *DSMathSpeechForTex(const std::string &tex, bool display);

NS_ASSUME_NONNULL_END
