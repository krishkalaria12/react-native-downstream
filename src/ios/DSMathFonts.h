#pragma once

#import <CoreGraphics/CoreGraphics.h>
#import <CoreText/CoreText.h>
#import <UIKit/UIKit.h>

#ifdef __cplusplus
#include "math/MathLayout.h"
#endif

NS_ASSUME_NONNULL_BEGIN

#ifdef __cplusplus
/// Tells the font loader where the math package's fonts are. Called by
/// `DownstreamInstallMathEngine`.
void DSMathInstallFontDirectory(const std::string &directory);
#endif

#ifdef __cplusplus
extern "C" {
#endif
/// Installs the math engine: RaTeX's `ratex_parse_and_layout`,
/// `ratex_free_display_list` and `ratex_get_last_error`, as addresses, and the
/// directory holding the KaTeX fonts.
///
/// Called from the math pod's `+load`, which declares this prototype itself: a
/// C function taking addresses is a surface a second pod can call without
/// seeing any of this library's headers, which are private.
void DownstreamInstallMathEngine(const void *parseAndLayout, const void *freeDisplayList, const void *lastError,
                                 const char *fontDirectory);
#ifdef __cplusplus
}
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

/// Draws `layout` at `size` points, with the top-left of its box at the
/// context's origin, and `color` as the color for every item the formula did not
/// color itself.
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

#ifdef __cplusplus
/// The attribute an inline formula's attachment character carries: the literal
/// it stands in for, delimiters included, which is what a copy writes back.
extern NSAttributedStringKey const DSMathLiteralAttributeName;

/// One inline formula, drawn in the line it sits in.
///
/// An attachment rather than a view, so TextKit wraps, selects and measures it
/// with the text around it. Its bounds put the formula's baseline on the line's
/// baseline, at the run's own size times KaTeX's 1.21, scaled down to fit a line
/// that is too narrow but never below half. It draws when TextKit asks, in the
/// run's own color resolved for the current appearance, so dark mode reaches it
/// without the text being rebuilt.
@interface DSMathAttachment : NSTextAttachment
- (instancetype)initWithLayout:(downstream::MathLayout::Handle)layout
                      fontSize:(CGFloat)fontSize
                         color:(UIColor *)color;
@end
#endif

/// The spoken form of a formula, from the engine's verbalizer.
///
/// A `NSString` rather than a synthesized attribute so a caller can use it for a
/// block's accessibility label and for an inline run's speech attribute without a
/// second entry point.
NSString *DSMathSpeechForTex(const std::string &tex, bool display);

NS_ASSUME_NONNULL_END
