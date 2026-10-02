// Engine checks for math sizing: the block height, the inline width and height,
// and the two KaTeX constants the engine copies.
//
//     node --test src/cpp/tests/engine.test.mjs   (runs test_math)
//
// `mathBlockHeight` here and `mathBlockHeight` in the Android mirror are the same
// expression written twice, so both are held to one table:
// `tests/android/MathLayoutCheck.kt` runs these cases against the Kotlin copy.
//
// Only the sizing is here. Whether a display list decodes is `test_display_list`,
// and whether a formula resolves is `test_math_blocks`; this binary answers the
// narrower question of what the numbers mean once a layout exists.

#include "math/MathLayout.h"
#include "tests/check.h"

#include <cmath>

using namespace downstream;

namespace {

/// Within a thousandth of a point, for the same reason `test_images.cpp` compares
/// that way: an assertion about a product in binary floating point is an assertion
/// about the arithmetic, not about the rule.
bool near(float actual, float expected) {
  const float scale = std::fabs(expected);
  return std::fabs(actual - expected) <= 1e-3f * (scale > 1 ? scale : 1);
}

void checkHeight(float actual, float expected, const char *label) {
  if (!near(actual, expected)) {
    std::fprintf(stderr, "FAIL %s: expected %.2f, actual %.2f\n", label, expected, actual);
    ds_check_failures++;
  }
}

void checkWidth(float actual, float expected, const char *label) {
  if (!near(actual, expected)) {
    std::fprintf(stderr, "FAIL %s: expected %.2f, actual %.2f\n", label, expected, actual);
    ds_check_failures++;
  }
}

/// A layout built by hand rather than through an engine, because this binary is
/// about the sizing of a layout and not about producing one. `height` above the
/// baseline and `depth` below it, both in em.
MathLayout layoutOf(float width, float height, float depth) {
  MathLayout layout;
  layout.width = width;
  layout.height = height;
  layout.depth = depth;
  return layout;
}

void testConstants() {
  // KaTeX's own `.katex { font-size: 1.21em }`. Not 1.2 and not 1.25: a formula
  // that is a size different from the reference one is a size different from
  // every document rendered with KaTeX, which is what these numbers are copied
  // for.
  checkHeight(kInlineMathScale, 1.21f, "the inline scale is KaTeX's 1.21em");
  checkHeight(kMinInlineScale, 0.5f, "the floor is half size");
}

void testBlockHeight() {
  const MathLayout layout = layoutOf(3, 0.9f, 0.3f);

  // Height and depth together, because a formula's box is the whole thing it
  // occupies: a tall radical or a deep fraction is the space above the baseline
  // and below it, not one or the other.
  checkHeight(mathBlockHeight(layout, 16, 0), 19.2f, "a block is height plus depth at the font size");
  checkHeight(mathBlockHeight(layout, 16, 8), 35.2f, "padding is added on both sides");
  checkHeight(mathBlockHeight(layout, 0, 0), 0, "no font and no padding is no height");

  // The font size is already scaled by the platform font scale, so a larger one
  // is a larger formula and nothing here re-scales it.
  checkHeight(mathBlockHeight(layout, 32, 0), 38.4f, "the font size is the scaled one, not re-scaled here");
  checkHeight(mathBlockHeight(layout, 24, 0), 28.8f, "1.5x the body is 1.5x the height");

  // A layout with no depth — a bare symbol, the common case — is its height.
  const MathLayout bare = layoutOf(1, 1, 0);
  checkHeight(mathBlockHeight(bare, 16, 0), 16, "a symbol with no depth is its own height");

  // A default-constructed layout is a real answer, not a crash: a formula whose
  // layout failed still takes room for its literal.
  const MathLayout empty;
  checkHeight(mathBlockHeight(empty, 16, 4), 8, "an unresolved layout is just its padding");
}

void testInlineMetrics() {
  const MathLayout layout = layoutOf(2.5f, 0.8f, 0.2f);

  // The width is the advance at the run's own size, with no scale applied here:
  // `inlineMathFont` already scaled the size by `kInlineMathScale`, so applying it
  // twice is how a formula ends up 1.46x the reference one.
  checkWidth(inlineMathWidth(layout, 16), 40, "an inline width is the advance at the run's size");
  checkWidth(inlineMathWidth(layout, 24), 60, "a larger run is a wider formula");

  // The height is the same height-and-depth pair a block takes, without the
  // block's padding, because an inline formula has no box around it.
  checkHeight(inlineMathHeight(layout, 16), 16, "an inline height is height plus depth");
  checkHeight(inlineMathHeight(layout, 0), 0, "a zero size is a zero height");

  // A formula's width does not depend on its depth, which is what lets a caller
  // break a line at a known point rather than measuring every candidate line.
  const MathLayout deep = layoutOf(2.5f, 0.8f, 5);
  checkWidth(inlineMathWidth(deep, 16), inlineMathWidth(layout, 16), "depth does not change a width");
}

} // namespace

int main() {
  testConstants();
  testBlockHeight();
  testInlineMetrics();
  return ds_check_result();
}