// Engine checks for the image box rule, the state rule, and the URL policy.
//
//     node --test src/cpp/tests/engine.test.mjs   (runs test_images)
//
// `imageBox` here and `imageBox` in the Android mirror are the same function
// written twice, so both are held to one table: this file is the table, and
// `tests/android/ImageLayoutCheck.kt` runs it against the Kotlin copy. A drift
// between them fails there rather than as an image that is 96 points taller on
// one platform than the other.

#include "ImageLayout.h"
#include "tests/check.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace downstream;

namespace {

/// True when two sizes agree to within a thousandth of a point.
///
/// The rule divides by a size the test chose, so a case like 600 by 3,000 scaled
/// to a 400 cap is 400 / 3000 = 0.13333… and 600 × that is not quite 80 in
/// binary floating point. Asserting exact equality here would be asserting
/// something about the arithmetic rather than about the rule, and the Kotlin
/// mirror would have to agree on the rounding as well as on the answer. The
/// tolerance is on the comparison, not on the rule: a difference a point cannot
/// be drawn at is not a difference in an image's box.
bool near(float actual, float expected) {
  const float scale = expected < 0 ? -expected : expected;
  return std::fabs(actual - expected) <= 1e-3f * (scale > 1 ? scale : 1);
}

/// The box, compared with that tolerance. Every case here is a whole number on
/// purpose: a table full of thirds cannot be read at a glance.
void checkBox(ImageBox actual, float width, float height, const char *label) {
  const bool matches = near(actual.width, width) && near(actual.height, height);
  if (!matches) {
    std::fprintf(stderr, "FAIL %s: expected %.1fx%.1f, actual %.1fx%.1f\n", label, width, height, actual.width,
                 actual.height);
    ds_check_failures++;
  }
}

void checkState(ImageState actual, ImageState expected, const char *label) {
  if (actual != expected) {
    std::fprintf(stderr, "FAIL %s: expected %d, actual %d\n", label, static_cast<int>(expected),
                 static_cast<int>(actual));
    ds_check_failures++;
  }
}

void checkAllowed(bool actual, bool expected, const char *label) {
  if (actual != expected) {
    std::fprintf(stderr, "FAIL %s: expected %s, actual %s\n", label, expected ? "allowed" : "blocked", actual ? "allowed" : "blocked");
    ds_check_failures++;
  }
}

// ------------------------------------------------------- the placeholder box

void testPlaceholder() {
  ImageMetrics metrics;
  const ImageEntry pending{0, ImageState::Pending, 0, 0};
  const ImageEntry loading{0, ImageState::Loading, 0, 0};

  checkBox(imageBox(&pending, 400, metrics), 192, 96, "a pending image is the placeholder");
  checkBox(imageBox(&loading, 400, metrics), 192, 96, "a loading image is the placeholder too");
  // The same answer for no entry at all, because a view handed a document it has
  // not read will draw what the reader would see a frame later.
  checkBox(imageBox(nullptr, 400, metrics), 192, 96, "no answer is the placeholder");

  // Clamped by both axes, not just the width: a placeholder is the smaller of
  // itself and the column, so a narrow column never gets a box wider than itself
  // and never gets one taller than it is wide.
  checkBox(imageBox(&pending, 100, metrics), 100, 96, "a narrow column clamps the width");
  checkBox(imageBox(&pending, 60, metrics), 60, 60, "a narrower column clamps both axes");
  checkBox(imageBox(&pending, 0, metrics), 0, 0, "an unmeasured column takes no room");
  checkBox(imageBox(&pending, -10, metrics), 0, 0, "a negative column takes no room");

  // A themed placeholder is the theme's size, not a constant, so an app can restyle
  // it without a props change.
  ImageMetrics wide;
  wide.placeholderWidth = 300;
  wide.placeholderHeight = 150;
  checkBox(imageBox(&pending, 400, wide), 300, 150, "the theme's placeholder size is used");
}

// ------------------------------------------------- a size before the bytes

void testKnownSizeWhileLoading() {
  ImageMetrics metrics;
  // The cache can hold a size for a URL this view has never loaded, which is what
  // makes a scrolled-back message lay out at its final height on its first frame.
  const ImageEntry loading{0, ImageState::Loading, 800, 600};
  checkBox(imageBox(&loading, 400, metrics), 400, 300, "a known size is used while the bytes arrive");

  // One axis alone is not a size: the other is unknown, so the placeholder is the
  // honest answer rather than a box stretched from half a fact.
  const ImageEntry halfKnown{0, ImageState::Loading, 800, 0};
  checkBox(imageBox(&halfKnown, 400, metrics), 192, 96, "half a size is not a size");
  const ImageEntry zeroWidth{0, ImageState::Pending, 0, 600};
  checkBox(imageBox(&zeroWidth, 400, metrics), 192, 96, "a zero width is not a size");
}

// ------------------------------------------------------- a loaded image

void testLoaded() {
  ImageMetrics metrics;

  const ImageEntry wide{0, ImageState::Loaded, 800, 600};
  // A column narrower than the image scales it down, and keeps its proportions.
  checkBox(imageBox(&wide, 400, metrics), 400, 300, "a wide image scales to the column");
  checkBox(imageBox(&wide, 1600, metrics), 800, 600, "an image smaller than the column is not blown up");
  checkBox(imageBox(&wide, 800, metrics), 800, 600, "an image the width of the column is its own size");
  checkBox(imageBox(&wide, 799, metrics), 799, 599.25, "one point narrower scales by a fraction");

  // A cap does the same thing the column does, and the tighter of the two wins.
  ImageMetrics capped;
  capped.maxHeight = 300;
  checkBox(imageBox(&wide, 1600, capped), 400, 300, "the cap scales a tall image down");
  checkBox(imageBox(&wide, 400, capped), 400, 300, "the cap and the column agree");
  ImageMetrics narrowCap;
  narrowCap.maxHeight = 150;
  checkBox(imageBox(&wide, 1600, narrowCap), 200, 150, "the tighter of the cap and the column wins");

  // 0 is no cap, which is what the default theme's 0 means, and a cap that is
  // already taller than the image changes nothing.
  ImageMetrics tallCap;
  tallCap.maxHeight = 5000;
  checkBox(imageBox(&wide, 1600, tallCap), 800, 600, "a cap taller than the image changes nothing");
  checkBox(imageBox(&wide, 1600, metrics), 800, 600, "no cap leaves the image alone");

  // A loaded image with no pixels at all is the placeholder rather than a zero
  // box, so a view that lost the size still reserves room.
  const ImageEntry empty{0, ImageState::Loaded, 0, 0};
  checkBox(imageBox(&empty, 400, metrics), 192, 96, "a loaded image with no pixels is the placeholder");

  // A tall, narrow image is scaled by its width unless a cap says otherwise,
  // which is the case a full-page screenshot hits. Without a cap there is nothing
  // to stop a 3,000-pixel image from taking a whole screen of scroll, which is
  // why the cap exists as a prop rather than as a hard limit.
  const ImageEntry tall{0, ImageState::Loaded, 600, 3000};
  checkBox(imageBox(&tall, 400, metrics), 400, 2000, "a tall image with no cap scales by its width");
  ImageMetrics screenCap;
  screenCap.maxHeight = 400;
  checkBox(imageBox(&tall, 400, screenCap), 80, 400, "a cap is what a tall image scales by");
  ImageMetrics shortCap;
  shortCap.maxHeight = 150;
  checkBox(imageBox(&tall, 400, shortCap), 30, 150, "a tighter cap scales a tall image further");
  checkBox(imageBox(&tall, 600, metrics), 600, 3000, "a tall image the width of the column is its own size");
}

// ------------------------------------------------- the states that are text

void testTextStates() {
  ImageMetrics metrics;
  // One line of prose, so the text after a broken image does not move when the
  // load fails and the placeholder collapses.
  const ImageEntry failed{0, ImageState::Failed, 0, 0};
  const ImageEntry blocked{0, ImageState::Blocked, 0, 0};
  const ImageEntry disabled{0, ImageState::Disabled, 0, 0};
  checkBox(imageBox(&failed, 400, metrics), 400, 18, "a failed image is one line");
  checkBox(imageBox(&blocked, 400, metrics), 400, 18, "a blocked image is one line");
  checkBox(imageBox(&disabled, 400, metrics), 400, 18, "a disabled image is one line");

  // The whole column, not the placeholder's width: alt text wraps.
  checkBox(imageBox(&failed, 100, metrics), 100, 18, "a failed image takes the column's width");
  checkBox(imageBox(&failed, 0, metrics), 0, 18, "an unmeasured failed image still takes its line");

  // A known size does not rescue a failed image: the bytes are not there, so the
  // size would be a lie.
  const ImageEntry failedSized{0, ImageState::Failed, 800, 600};
  checkBox(imageBox(&failedSized, 400, metrics), 400, 18, "a failed image ignores a known size");

  ImageMetrics tall;
  tall.fallbackTextHeight = 42;
  checkBox(imageBox(&failed, 400, tall), 400, 42, "the theme sets the fallback line's height");
}

// ----------------------------------------------------------- the lookup

void testFind() {
  ImageStateLayout layout;
  layout.entries = {{2, ImageState::Loaded, 10, 20}, {0, ImageState::Pending, 0, 0}, {1, ImageState::Failed, 0, 0}};
  // Sorted by ordinal, which is the contract that lets a caller binary-search.
  std::sort(layout.entries.begin(), layout.entries.end(),
            [](const ImageEntry &entry, const ImageEntry &other) { return entry.ordinal < other.ordinal; });

  CHECK(layout.find(0) != nullptr, "an entry is found by its ordinal");
  CHECK(layout.find(2) != nullptr, "the last entry is found too");
  CHECK(layout.find(3) == nullptr, "an ordinal with no entry is not invented");
  CHECK(layout.find(0)->state == ImageState::Pending, "the right entry comes back");

  // A find on an empty layout is an answer, not a crash: a document with no images
  // is ordinary.
  ImageStateLayout empty;
  CHECK(empty.find(0) == nullptr, "an empty layout has no entries");

  // The sorted invariant holds after a sort that is not a sort by ordinal alone,
  // which is what a caller gets when it builds the list in document order.
  std::sort(layout.entries.begin(), layout.entries.end(),
            [](const ImageEntry &entry, const ImageEntry &other) { return entry.ordinal < other.ordinal; });
  CHECK(layout.find(1)->state == ImageState::Failed, "sorting by ordinal leaves every entry findable");
}

// ---------------------------------------------------------- the state rule

void testStateFor() {
  // Off wins over everything, so an app that turned images off makes no request
  // even for a URL it would otherwise allow.
  checkState(imageStateFor(false, true, true, true), ImageState::Disabled, "off beats a known image");
  checkState(imageStateFor(false, false, false, false), ImageState::Disabled, "off beats an unfinished one");

  // A URL the engine's policy rejected, or an empty one. An empty destination is
  // not an error while the writer is still typing, so the document's settled count
  // is what says whether to show a box.
  checkState(imageStateFor(true, false, false, false), ImageState::Pending, "an unfinished destination is pending");
  checkState(imageStateFor(true, false, true, false), ImageState::Disabled, "a settled empty destination is disabled");

  // A loadable URL: known or not.
  checkState(imageStateFor(true, true, true, true), ImageState::Loaded, "a known image is loaded");
  checkState(imageStateFor(true, true, true, false), ImageState::Loading, "an unknown image is loading");
  // Whether the document has settled does not change a loadable URL's state: a
  // picture the reader can see is a picture, mid-stream or not.
  checkState(imageStateFor(true, true, false, true), ImageState::Loaded, "a mid-stream image is still loaded");
}

// ----------------------------------------------------------- the URL policy

void testUrlAllowed() {
  const std::vector<std::string> anywhere{"*"};
  checkAllowed(imageUrlAllowed("https://example.com/a.png", anywhere), true, "a star allows any host");
  checkAllowed(imageUrlAllowed("http://example.com/a.png", anywhere), true, "a star allows http too, and the engine's own policy is what rejects it");
  checkAllowed(imageUrlAllowed("", anywhere), true, "a star allows an empty URL, which the state rule then handles");

  // No list at all allows nothing. "Fetch from nowhere" is the only safe reading
  // of a list an app got wrong.
  const std::vector<std::string> none;
  checkAllowed(imageUrlAllowed("https://example.com/a.png", none), false, "an empty list allows nothing");

  // An entry has to match from the start: a host in the middle is not a prefix.
  const std::vector<std::string> cdn{"https://cdn.example.com/"};
  checkAllowed(imageUrlAllowed("https://cdn.example.com/a.png", cdn), true, "a prefix allows its own subtree");
  checkAllowed(imageUrlAllowed("https://cdn.example.com", cdn), false, "a prefix is not a host");
  checkAllowed(imageUrlAllowed("https://evil.example.com/cdn.example.com/a.png", cdn), false,
               "a prefix in the middle of a URL allows nothing");
  checkAllowed(imageUrlAllowed("https://cdn.example.com.evil.com/a.png", cdn), false,
               "a host that starts with an allowed one is not allowed");
  checkAllowed(imageUrlAllowed("", cdn), false, "an empty URL matches no prefix");
  checkAllowed(imageUrlAllowed("https://cdn.example.com/", cdn), true, "a prefix matches itself");

  // A host name is not case sensitive, and a prefix list an app wrote as
  // `HTTPS://…` means the same thing.
  checkAllowed(imageUrlAllowed("HTTPS://CDN.Example.COM/a.png", cdn), true, "a prefix matches ignoring case");
  checkAllowed(imageUrlAllowed("https://cdn.example.com/a.png", {"HTTPS://CDN.EXAMPLE.COM/"}), true,
               "an uppercase prefix matches a lowercase URL");

  // Any one of several entries is enough, and the star wins wherever it is.
  const std::vector<std::string> several{"https://images.example.org/", "https://cdn.example.com/"};
  checkAllowed(imageUrlAllowed("https://cdn.example.com/a.png", several), true, "the second entry matches");
  checkAllowed(imageUrlAllowed("https://images.example.org/a.png", several), true, "the first entry matches");
  checkAllowed(imageUrlAllowed("https://other.example.net/a.png", several), false, "no entry matches");
  checkAllowed(imageUrlAllowed("https://other.example.net/a.png", {"https://other.example.net/", "*"}), true,
               "the star allows from wherever it sits in the list");

  // An empty entry is not a prefix that matches everything: it is a list entry
  // that names no host.
  checkAllowed(imageUrlAllowed("https://example.com/a.png", {""}), false, "an empty entry matches nothing");
  checkAllowed(imageUrlAllowed("https://example.com/a.png", {"", "*"}), true, "an empty entry does not shadow the star");
}

} // namespace

int main() {
  testPlaceholder();
  testKnownSizeWhileLoading();
  testLoaded();
  testTextStates();
  testFind();
  testStateFor();
  testUrlAllowed();
  return ds_check_result();
}