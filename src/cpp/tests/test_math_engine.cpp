// The math seam against the real engine: RaTeX's own C functions, installed the
// way the math package installs them, with their display lists read by the
// decoder both platforms draw from.
//
//     node --test src/cpp/tests/engine.test.mjs   (runs test_math_engine)
//
// Linked against the macOS slice that `math/scripts/vendor-ratex.mjs` writes to
// `math/vendor/host`. Nothing here restates what RaTeX returns: every expected
// number is a property of the formula (a fraction has one bar, `\color{red}` is
// red, an unfinished `\frac` fails), so a RaTeX version bump that moves a glyph
// does not break a test, and a decoder that misreads the protocol does.

#include "math/MathDisplayList.h"
#include "math/MathEngine.h"
#include "tests/check.h"

// RaTeX's own header, from the vendored slice, so the core's mirror of its ABI is
// checked against the real thing rather than against a second copy.
#include "../../../math/vendor/host/Headers/ratex.h"

#include <atomic>
#include <cstddef>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using namespace downstream;

// The core mirrors RaTeX's ABI so it can compile without the package; these are
// what make the mirror the same layout as the header the package links.
static_assert(sizeof(MathEngineColor) == sizeof(RatexColor));
static_assert(offsetof(MathEngineColor, a) == offsetof(RatexColor, a));
static_assert(sizeof(MathEngineOptions) == sizeof(RatexOptions));
static_assert(offsetof(MathEngineOptions, structSize) == offsetof(RatexOptions, struct_size));
static_assert(offsetof(MathEngineOptions, displayMode) == offsetof(RatexOptions, display_mode));
static_assert(offsetof(MathEngineOptions, color) == offsetof(RatexOptions, color));
static_assert(sizeof(MathEngineResult) == sizeof(RatexResult));
static_assert(offsetof(MathEngineResult, data) == offsetof(RatexResult, data));
static_assert(offsetof(MathEngineResult, errorCode) == offsetof(RatexResult, error_code));

namespace {

/// RaTeX's own symbols, installed the way both platforms install them.
MathEngineFunctions ratex() {
  MathEngineFunctions functions{};
  mathEngineFunctionsFromSymbols(reinterpret_cast<const void *>(&ratex_parse_and_layout),
                                 reinterpret_cast<const void *>(&ratex_free_display_list),
                                 reinterpret_cast<const void *>(&ratex_get_last_error), &functions);
  return functions;
}

bool resolveRatex(MathEngineFunctions *out) {
  *out = ratex();
  return true;
}

bool resolveNothing(MathEngineFunctions * /*out*/) { return false; }

MathLayout::Handle layout(const char *tex, bool display, std::string *error = nullptr) {
  std::string ignored;
  return MathEngine::layout(tex, display, error != nullptr ? error : &ignored);
}

void fraction() {
  const MathLayout::Handle frac = layout("\\frac{1}{2}", true);
  CHECK(frac != nullptr, "a fraction lays out");
  if (!frac) {
    return;
  }
  CHECK(frac->glyphs.size() == 2, "a fraction has a numerator and a denominator");
  CHECK(frac->rules.size() == 1, "a fraction has one bar");
  CHECK(frac->width > 0 && frac->height > 0 && frac->depth > 0, "a display fraction has a box above and below");
  if (frac->rules.size() == 1) {
    const MathRule &bar = frac->rules.front();
    // A `Line` is its centre line and a thickness. The rule is the box around it,
    // so it has a height, and it sits between the two digits.
    CHECK(bar.height > 0.01f && bar.height < 0.2f, "the bar is as thick as the line RaTeX drew");
    CHECK(bar.width > 0.3f, "the bar spans the digits");
    CHECK(bar.x > 0, "the bar starts where the line does, not at the origin");
    CHECK(frac->glyphs.size() == 2 && bar.y > frac->glyphs[0].y && bar.y < frac->glyphs[1].y,
          "the bar lies between the numerator's baseline and the denominator's");
    CHECK(!bar.dashed, "a fraction bar is solid");
    CHECK(bar.color == kInheritColor, "an uncolored bar inherits the text color");
  }
  for (const MathGlyph &glyph : frac->glyphs) {
    CHECK(glyph.font == MathFont::MainRegular, "digits are Main-Regular");
    CHECK(glyph.color == kInheritColor, "an uncolored glyph inherits the text color");
  }
  CHECK(frac->glyphs.size() == 2 && frac->glyphs[0].codePoint == U'1' && frac->glyphs[1].codePoint == U'2',
        "the glyphs are the digits");
}

void color() {
  const MathLayout::Handle red = layout("x+\\color{red}{y}", false);
  CHECK(red != nullptr, "a colored formula lays out");
  if (!red) {
    return;
  }
  bool sawRed = false;
  bool sawInherit = false;
  for (const MathGlyph &glyph : red->glyphs) {
    if (glyph.codePoint == U'y') {
      sawRed = glyph.color == 0xFFFF0000u;
    }
    if (glyph.codePoint == U'x') {
      sawInherit = glyph.color == kInheritColor;
    }
  }
  CHECK(sawRed, "\\color{red} comes through as opaque red, not as a near-black byte");
  CHECK(sawInherit, "the uncolored x keeps inheriting");
}

void fontsAndFallback() {
  const MathLayout::Handle italic = layout("x", false);
  CHECK(italic && italic->glyphs.size() == 1 && italic->glyphs[0].font == MathFont::MathItalic,
        "a variable is Math-Italic");
  const MathLayout::Handle cjk = layout("x \\text{中}", false);
  bool system = false;
  if (cjk) {
    for (const MathGlyph &glyph : cjk->glyphs) {
      system = system || (glyph.codePoint == U'中' && glyph.font == MathFont::System);
    }
  }
  CHECK(system, "a character KaTeX has no glyph for draws in the platform font");
  const MathLayout::Handle big = layout("\\sum_{i=1}^{n} i", true);
  bool size = false;
  if (big) {
    for (const MathGlyph &glyph : big->glyphs) {
      size = size || glyph.font == MathFont::Size2 || glyph.font == MathFont::Size1;
    }
  }
  CHECK(size, "a display sum uses a Size font");
}

void paths() {
  const MathLayout::Handle brace = layout("\\overbrace{abc}", true);
  CHECK(brace != nullptr, "an overbrace lays out");
  if (!brace) {
    return;
  }
  CHECK(!brace->paths.empty(), "an overbrace is drawn as a path");
  for (const MathPath &path : brace->paths) {
    CHECK(!path.commands.empty(), "the path keeps its commands");
    CHECK(path.commands.empty() || path.commands.front().op == PathOp::Move, "a path starts with a move");
    bool lines = false;
    for (const PathCommand &command : path.commands) {
      lines = lines || command.op == PathOp::Line;
    }
    CHECK(lines, "LineTo commands are read as lines");
  }
}

void dashedAndRects() {
  const MathLayout::Handle box = layout("\\colorbox{yellow}{x}", false);
  bool filled = false;
  if (box) {
    for (const MathRule &rule : box->rules) {
      filled = filled || (rule.height > 0.5f && rule.width > 0.5f && rule.color != kInheritColor);
    }
  }
  CHECK(filled, "a colorbox is a colored rect with its own height");
  const MathLayout::Handle dashed = layout("\\begin{array}{cc} a & b \\\\ \\hdashline c & d \\end{array}", true);
  bool sawDash = false;
  if (dashed) {
    for (const MathRule &rule : dashed->rules) {
      sawDash = sawDash || rule.dashed;
    }
  }
  // RaTeX 0.1.14 draws \hdashline as a solid rect, so this only checks that a
  // `dashed: true` would not fail the formula: the array lays out either way.
  CHECK(dashed != nullptr, "an array with a dashed line lays out");
  (void)sawDash;
}

void failures() {
  MathEngine::clearCache();
  std::string error;
  const MathLayout::Handle broken = layout("\\frac{a}{", true, &error);
  CHECK(broken == nullptr, "an unfinished fraction fails");
  CHECK(error.find("ParseError") != std::string::npos, "the failure carries RaTeX's own message");
  for (int i = 0; i < 40; i++) {
    std::string repeated;
    CHECK(layout("\\frac{a}{", true, &repeated) == nullptr && repeated == error,
          "repeated failures preserve RaTeX's original message");
  }
  CHECK(MathEngine::engineCallCount() == 1, "identical invalid formulas reach RaTeX once");
  CHECK(MathEngine::cachedCount() == 1, "RaTeX failures occupy one bounded cache entry");
  CHECK(layout("\\frac{a}{", false) == nullptr, "an inline invalid formula still fails");
  CHECK(MathEngine::engineCallCount() == 2, "inline and display failures are cached separately");
  const MathLayout::Handle completed = layout("\\frac{a}{b}", true);
  CHECK(completed != nullptr && MathEngine::engineCallCount() == 3,
        "completing rejected TeX evaluates its new source");
  std::string unknown;
  CHECK(layout("\\notacommand", false, &unknown) == nullptr, "an unknown command fails rather than drawing");
  CHECK(!unknown.empty(), "and says why");
}

void caching() {
  MathEngine::clearCache();
  const MathLayout::Handle first = layout("a^2+b^2=c^2", false);
  const uint64_t calls = MathEngine::engineCallCount();
  const MathLayout::Handle second = layout("a^2+b^2=c^2", false);
  CHECK(first != nullptr && first == second, "the same formula is one shared layout");
  CHECK(MathEngine::engineCallCount() == calls, "and one engine call");
  const MathLayout::Handle display = layout("a^2+b^2=c^2", true);
  CHECK(display != nullptr && display != first, "display style is its own entry");
}

void parallel() {
  // RaTeX keeps its error slot per thread, so the pool lays formulas out at once.
  // Each thread checks its own failure message, which a shared slot would mix up.
  MathEngine::clearCache();
  std::atomic<int> wrong{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; t++) {
    threads.emplace_back([t, &wrong] {
      for (int i = 0; i < 40; i++) {
        const std::string tex = "x_{" + std::to_string(t * 100 + i) + "}";
        if (!layout(tex.c_str(), i % 2 == 0)) {
          wrong++;
        }
        std::string error;
        if (layout("\\frac{", false, &error) != nullptr || error.find("ParseError") == std::string::npos) {
          wrong++;
        }
      }
    });
  }
  for (std::thread &thread : threads) {
    thread.join();
  }
  CHECK(wrong.load() == 0, "parallel layouts and failures each get their own answer");
}

void resolvers() {
  MathEngine::install(MathEngineFunctions{}, "");
  CHECK(!MathEngine::available(), "an empty install is no engine");
  std::string error;
  CHECK(layout("x", false, &error) == nullptr, "and lays nothing out");

  MathEngine::installResolver(resolveNothing, "fonts");
  CHECK(MathEngine::available(), "a registered resolver is an installed package");
  error.clear();
  CHECK(layout("x", false, &error) == nullptr, "a resolver that finds no library lays nothing out");
  CHECK(!MathEngine::available(), "and stops reporting an engine");

  MathEngine::clearCache();
  MathEngine::installResolver(resolveRatex, "fonts");
  CHECK(MathEngine::engineCallCount() == 0, "registering loads nothing");
  CHECK(layout("y", false) != nullptr, "the first formula resolves the engine");
  CHECK(MathEngine::fontDirectory() == "fonts", "the resolver keeps its font directory");
}

} // namespace

int main() {
  MathEngine::install(ratex(), "fonts");
  fraction();
  color();
  fontsAndFallback();
  paths();
  dashedAndRects();
  failures();
  caching();
  parallel();
  resolvers();
  return ds_check_result();
}
