#pragma once

// A laid-out formula: the data both platforms draw.
//
// The engine hands this out instead of a view, which is what lets one layout
// serve measurement, mounting, an inline attachment and a block view on two
// different platforms. Nothing here is a drawable: units are em, and the
// baseline is at y = height, so the same list serves every font size, every
// font scale, and both appearances without being laid out again.
//
// A layout never carries a theme. The engine tells the math engine that the
// default color is a sentinel, and the decoder maps exactly that value to
// `kInheritColor`, so an item that the formula did not color itself draws in
// whatever color the run it sits in already has. Dark mode, a theme, and
// increased contrast therefore reach a formula without touching the layout.

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
namespace downstream {

/**
 * The fonts KaTeX ships and RaTeX names in its display list.
 *
 * `System` is the platform font rather than a KaTeX face: RaTeX routes a code
 * point it has no glyph for to it, which is how a CJK character or an emoji
 * inside a formula renders. The order is RaTeX's, and a decoder maps a name it
 * does not know onto `System` rather than dropping the item, so a version bump
 * that adds a font degrades to the platform face instead of to nothing.
 */
enum class MathFont : uint8_t {
  MainRegular = 0,
  MainBold,
  MainItalic,
  MainBoldItalic,
  MathItalic,
  MathBoldItalic,
  AMSRegular,
  CaligraphicRegular,
  CaligraphicBold,
  FrakturRegular,
  FrakturBold,
  SansSerifRegular,
  SansSerifBold,
  SansSerifItalic,
  ScriptRegular,
  TypewriterRegular,
  Size1,
  Size2,
  Size3,
  Size4,
  System,
};

/// The file name of a math font inside the package's fonts directory.
std::string_view mathFontFileName(MathFont font);

/// The KaTeX font family a face belongs to, for CoreText and Android.
std::string_view mathFontPostScriptName(MathFont font);

/// RaTeX's own name for a font, which is what its display list carries.
std::string_view mathFontName(MathFont font);

/**
 * The color a platform substitutes for the text it draws beside the formula.
 *
 * Zero rather than a sentinel bit pattern, because it is the value the engine
 * asked the math engine for and the only value the decoder can recognize: any
 * other default would be a color a formula could legitimately ask for.
 */
inline constexpr uint32_t kInheritColor = 0;

/// A path segment: a move, a line, a curve, or a close.
enum class PathOp : uint8_t {
  Move = 0,
  Line,
  Quad,
  Cubic,
  Close,
};

/// One path segment, in em relative to its path's origin. `x` and `y` are the
/// end point; `x1, y1` is a quadratic's control point and a cubic's first one,
/// and `x2, y2` is a cubic's second.
struct PathCommand {
  PathOp op{PathOp::Move};
  float x{0}, y{0};
  float x1{0}, y1{0};
  float x2{0}, y2{0};
};

/// One glyph, placed in em relative to the layout's origin.
struct MathGlyph {
  float x{0};
  float y{0};
  /// A size-2 or larger glyph is emitted as a scaled glyph rather than as a
  /// path, so a large `\sum` stays a glyph of Size1 at twice the size.
  float scale{1};
  MathFont font{MathFont::MainRegular};
  char32_t codePoint{0};
  uint32_t color{kInheritColor};
};

/// A filled rectangle: a fraction bar, a radical's overbar, a `\rule`.
///
/// The box itself, top-left and size. RaTeX sends a `Line` as its centre line and
/// a thickness; the decoder turns that into this box so a platform draws one
/// shape for both.
struct MathRule {
  float x{0}, y{0}, width{0}, height{0};
  uint32_t color{kInheritColor};
  /// A dashed rule, which is what a `\hdashline` draws.
  bool dashed{false};
};

/// A stroked or filled outline, in em. Used for radicals, stretchy delimiters
/// and the symbols KaTeX draws as a path rather than as a glyph.
struct MathPath {
  float x{0}, y{0};
  std::vector<PathCommand> commands;
  /// A stroked path's width in em, or zero for the platform's hairline.
  float strokeWidth{0};
  bool fill{false};
  uint32_t color{kInheritColor};
};

/**
 * One formula's display list, immutable and shared.
 *
 * A handle rather than a value because a formula's layout is stored on the
 * document, is measured by a shadow node on one thread and drawn by a view on
 * another, and is reused by every snapshot that holds the same TeX. Sharing is
 * what makes that safe, and the shared cache is what makes it cheap.
 */
struct MathLayout {
  using Handle = std::shared_ptr<const MathLayout>;

  /// Advance, height above the baseline, and depth below it, all in em. The
  /// baseline sits at y = height, so a glyph's y is measured from the top of
  /// the box rather than from the baseline.
  float width{0};
  float height{0};
  float depth{0};

  std::vector<MathGlyph> glyphs;
  std::vector<MathRule> rules;
  std::vector<MathPath> paths;

  /// Total drawable items, so a caller can budget and a test can assert that a
  /// formula drew something without walking three vectors.
  size_t itemCount() const { return glyphs.size() + rules.size() + paths.size(); }
};

/**
 * The height of one math block, in points.
 *
 * The one definition both platforms' segment height functions call, so a
 * measured block and a drawn block cannot be different sizes. `fontSize` is
 * already scaled by the platform font scale, and `padding` is the theme's
 * vertical padding applied on both sides.
 */
inline float mathBlockHeight(const MathLayout &layout, float fontSize, float padding) {
  return (layout.height + layout.depth) * fontSize + 2 * padding;
}

/**
 * The size an inline formula draws at, in points.
 *
 * KaTeX's own `.katex { font-size: 1.21em }`, so a formula in a heading is a
 * heading-sized formula rather than a paragraph-sized one inside a heading.
 * Applied to the run's own scaled size, which is what makes it follow Dynamic
 * Type.
 */
inline constexpr float kInlineMathScale = 1.21f;

/**
 * The width of one inline formula, in points, and the scale it draws at.
 *
 * A formula wider than the line is scaled down so it still fits, never below
 * `kMinInlineScale`: past that a formula is unreadable and clipping it at the
 * segment edge is more honest than drawing it at a third of its size. A caller
 * that gets a scale below one draws outside its own bounds.
 */
inline constexpr float kMinInlineScale = 0.5f;

inline float inlineMathWidth(const MathLayout &layout, float fontSize) {
  return layout.width * fontSize;
}

inline float inlineMathHeight(const MathLayout &layout, float fontSize) {
  return (layout.height + layout.depth) * fontSize;
}

inline float inlineMathAscent(const MathLayout &layout, float fontSize) {
  return layout.height * fontSize;
}

inline float inlineMathDescent(const MathLayout &layout, float fontSize) {
  return layout.depth * fontSize;
}

} // namespace downstream
