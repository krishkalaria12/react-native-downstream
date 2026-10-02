// Host test driver: prints the engine's eased alpha and rise for a fixed table of
// curves, durations and offsets.
//
// The Kotlin mirror in `src/android/src/main/java/com/downstream/Reveal.kt`
// evaluates the same curve on the drawing thread, and a host JVM cannot run the
// platform's `PathInterpolator`, so the mirror's own solver is what
// `tests/android/RevealCheck.kt` checks against these numbers. The bound the check
// applies is 0.002, which is below what an eighth of an alpha step can show.
//
// Output is one line per case: `x1 y1 x2 y2 duration offset alpha rise`, with
// alpha and rise to six decimal places. Both platforms' drawing passes read
// nothing else per frame, so this table is the whole of what has to agree.

#include "Reveal.h"

#include <cstdio>

using namespace downstream;

namespace {

// Every CSS keyword the public API accepts, in the order
// `src/animation.ts` parses them, plus one custom curve and one that overshoots
// vertically, which CSS allows and a solver must not reject.
constexpr CubicBezier kCurves[] = {
    {0.0f, 0.0f, 1.0f, 1.0f},   // linear
    {0.25f, 0.1f, 0.25f, 1.0f}, // ease
    {0.42f, 0.0f, 1.0f, 1.0f},  // ease-in
    {0.0f, 0.0f, 0.58f, 1.0f},  // ease-out
    {0.42f, 0.0f, 0.58f, 1.0f}, // ease-in-out
    {0.68f, -0.55f, 0.27f, 1.55f}, // cubic-bezier, overshooting
};

// The two ends and a few points through, so a check sees the clamps and the
// easing's own shape rather than only its midpoint.
constexpr double kOffsets[] = {-40.0, 0.0, 1.0, 37.5, 75.0, 112.5, 149.0, 150.0, 400.0};

// The two durations the public API's range makes interesting: the default and one
// short enough that a fast stream's units overlap heavily.
constexpr float kDurations[] = {150.0f, 40.0f};

} // namespace

int main() {
  for (const CubicBezier &curve : kCurves) {
    for (const float duration : kDurations) {
      RevealOptions options;
      // `rise` answers zero for anything but a `Rise`, so the table is dumped with
      // a `Rise`. A `Rise` is a fade plus a transform, so the alpha column is still
      // the fade's own.
      options.effect = RevealEffect::Rise;
      options.durationMs = duration;
      options.easing = curve;
      for (const double elapsed : kOffsets) {
        // A unit that started at zero, so `elapsed` is both the offset from its
        // start and the time the alpha is read at.
        RevealUnit unit;
        unit.block = 0;
        unit.start = 0;
        unit.end = 1;
        unit.startAt = 0;
        const double now = elapsed;
        std::printf("%.6f %.6f %.6f %.6f %.1f %.1f %.6f %.6f\n", curve.x1, curve.y1, curve.x2, curve.y2, duration,
                    elapsed, static_cast<double>(revealAlpha(unit, now, options)),
                    static_cast<double>(revealRise(unit, now, options)));
      }
    }
  }
  return 0;
}
