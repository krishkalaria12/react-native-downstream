#pragma once

// One rule for the box an image takes, and the per-view state that feeds it.
//
// The height of an image is not a property of the image: it is a function of
// the column it is drawn in, whether its size is known yet, whether it failed,
// and whether the app's prefix list allows fetching it at all. All four change
// after the document was parsed, and none of them change the document. So they
// travel beside it, in the layout hint and the mounting state, exactly as
// `CodeLayout` travels, and `imageBox` is the only function that turns them
// into a size.
//
// One function rather than one per platform, because the alternative is two
// implementations that agree until the day one of them is edited. Android
// mirrors it in Kotlin, and `pnpm test:kotlin` checks the mirror against this
// one table, so a divergence is a failing test rather than a wrong height.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace downstream {

/// What a view knows about one image right now.
enum class ImageState : uint8_t {
  /// The destination is still arriving, or no request has started. Streamdown's
  /// pulsing box.
  Pending = 0,
  /// A request is in flight and its size is not known yet.
  Loading,
  /// Loaded, with a size.
  Loaded,
  /// The request failed, or the bytes are not something either platform's image
  /// pipeline decodes, which for SVG means the same thing.
  Failed,
  /// The prefix list does not allow this URL, so no request was ever made.
  Blocked,
  /// The `images` prop is off, or the block has not settled, so an unfinished
  /// destination is not yet a request.
  Disabled,
};

/// One image's state in one view. The unit of `ImageStateLayout::entries`.
struct ImageEntry {
  /// The image's index in `Document::images`. Ordinals are counted as the
  /// document is built, so they are stable across append-only snapshots and a
  /// view can match a state to an image without knowing where the image sits.
  uint32_t ordinal{0};
  ImageState state{ImageState::Pending};
  /// The image's own pixels. `Loaded` only, and never scaled: the point size is
  /// derived from it, and the same image measured twice has to measure the same
  /// both times.
  float pixelWidth{0};
  float pixelHeight{0};

  bool operator==(const ImageEntry &) const = default;
};

/**
 * Every image answer one view has.
 *
 * This is what the mounting state carries and what a view draws from, so it is
 * exactly what both need: whether images are on, and each image's state and
 * size. It is kept sorted by ordinal so a caller binary-searches for the entry
 * it wants, which is the reasoning the code layout's choice sets use too.
 */
struct ImageStateLayout {
  /// The `images` prop. False means no request is ever made and every image
  /// draws its alt text, which is one case rather than a flag the box rule
  /// re-reads per image.
  bool enabled{true};
  std::vector<ImageEntry> entries;

  bool operator==(const ImageStateLayout &) const = default;

  /// The entry for `ordinal`, or null when the view has no answer for it.
  const ImageEntry *find(uint32_t ordinal) const;
};

/**
 * What measurement needs: the answers, plus the policy that produces them.
 *
 * The prefixes live here rather than in the state because only a measurement
 * has to decide whether a URL may be fetched — the view is handed a state that
 * already says `Blocked` — so the round trip through Android's JNI carries
 * entries alone. Changing the prefix list therefore changes an entry, which is
 * what makes the shadow node notice, rather than a field nothing draws from.
 */
struct ImageLayout {
  ImageStateLayout state;
  std::vector<std::string> allowedPrefixes;

  bool operator==(const ImageLayout &) const = default;
};

/// The placeholder box, as Streamdown's `h-24 w-48`: 96 by 192 points at 16
/// points to the rem, which is a phone's default root font size.
inline constexpr float kImagePlaceholderWidth = 192;
inline constexpr float kImagePlaceholderHeight = 96;

/// One line of fallback text at a platform's own line height, so a broken image
/// takes the room of a line of prose and the text after it does not move.
inline constexpr float kImageFallbackLineHeight = 18;

/**
 * The theme's image metrics, in points.
 *
 * The values that change an image's box. They come from the theme rather than
 * from props, so an app restyles an image without a props change, and a view
 * that passes no theme gets these defaults.
 */
struct ImageMetrics {
  /// A cap on a loaded image's height. 0, the default, is no cap: an image is
  /// scaled to the column and to this, and never up.
  float maxHeight{0};
  /// The corner radius. Drawn, never measured, so a radius never changes a
  /// height.
  float borderRadius{8};
  /// Vertical space outside the image, drawn above and below it.
  float marginTop{0};
  float marginBottom{0};
  /// One line of fallback text, which is what a failed, blocked, or disabled
  /// image takes. Defaulted from `kImageFallbackLineHeight`.
  float fallbackTextHeight{kImageFallbackLineHeight};
  /// The placeholder's size, clamped to the column. Defaulted from
  /// `kImagePlaceholderWidth` and `kImagePlaceholderHeight`.
  float placeholderWidth{kImagePlaceholderWidth};
  float placeholderHeight{kImagePlaceholderHeight};
};

/**
 * The placeholder a pending or loading image shows, and one line of text.
 *
 * Named rather than `Size`, because `facebook::react::Size` is in scope in the
 * same translation units and two structs with one name make every unqualified
 * use ambiguous.
 */
struct ImageBox {
  float width{0};
  float height{0};

  bool operator==(const ImageBox &) const = default;
};

/**
 * The box one image takes in a column, from its state and the theme's metrics.
 *
 * The only place an image's height is decided. The shadow node calls it while
 * measuring and both platform views call it while drawing, so a measured image
 * and a drawn image cannot be different sizes.
 *
 * A pending or loading image with no known size is the placeholder, clamped to
 * the column so a narrow column never gets a box wider than itself. A loaded
 * image is its own pixels, scaled down to the column and to `maxHeight` and
 * never up: an image smaller than the column is not blown up, because a
 * 64-pixel icon at four times its size is a blur and not the picture. A failed,
 * blocked, or disabled image is one line of fallback text, so a broken image
 * takes the room of a line rather than of a placeholder the reader cannot use.
 */
ImageBox imageBox(const ImageEntry *entry, float columnWidth, const ImageMetrics &metrics);

/**
 * The state one image takes, given the view's policy and what the engine knows
 * about the URL.
 *
 * Separate from `imageBox` so a caller that needs the state — an accessibility
 * label, a request that must not start — asks the same question the box does.
 *
 * `settled` is the document's own: a destination that is still arriving is
 * `Pending`, and a settled empty one is simply not loadable. That is the only
 * difference between "the writer has not finished typing the URL" and "the
 * writer wrote no URL", and it is why an empty image at the end of a finished
 * stream shows its alt text rather than a box that never fills.
 */
ImageState imageStateFor(bool enabled, bool urlLoadable, bool settled, bool known);

/**
 * Whether `url` may be fetched, under `prefixes`.
 *
 * rehype-harden's rule, which is also Streamdown's: `*` allows any host, and
 * otherwise a URL matches when it starts with one of the prefixes, compared
 * case-insensitively. A prefix list with no `*` and no entries allows nothing,
 * because "fetch from nowhere" is the only safe reading of a list an app got
 * wrong.
 */
bool imageUrlAllowed(std::string_view url, const std::vector<std::string> &prefixes);

} // namespace downstream
