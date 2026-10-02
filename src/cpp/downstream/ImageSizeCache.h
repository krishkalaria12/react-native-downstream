#pragma once

// What the loader already learned about a URL, kept past the view that learned
// it.
//
// A load reports its size once, and by the time it does the view that asked may
// already be recycled. Keeping the answer here rather than on the view is what
// lets a message scrolled back into view — or a second message showing the same
// chart — lay out at its final height on its first frame instead of showing a
// placeholder and then jumping.
//
// It is a cache, not a store: bounded by count, and a miss costs one request.
// The bound is generous enough for a long scrollback (a 50-image history) and
// small enough that it is not a memory leak, which matters because the only
// thing it holds per entry is two floats and a URL the document already holds
// anyway.

#include "ImageLayout.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace downstream {

/// One URL's answer.
struct CachedImageSize {
  ImageState state{ImageState::Loading};
  float pixelWidth{0};
  float pixelHeight{0};
};

/// The process-wide record of what images have loaded, failed, or not finished.
///
/// One instance, guarded internally, and safe to call from any thread: a load
/// completes on a loader's queue while a shadow node measures on the main
/// thread, and those two must not race on the same map.
class ImageSizeCache {
public:
  /// The bound. A scrollback past this re-fetches an image the reader has
  /// already seen, which is a cost the reader sees and a leak is not.
  static constexpr size_t kMaxEntries = 1024;

  /// The answer for `url`, or null when this process has none. A `Loaded`
  /// answer carries the pixels; a `Failed` answer carries none and stops the
  /// request from being made again for a message that is still on screen.
  static const CachedImageSize *find(std::string_view url);

  /// Records a load that finished. An entry already recorded is not replaced by
  /// a weaker one: a second request that has not finished yet must not erase the
  /// size a first one already reported, or a message would fall back to its
  /// placeholder for no reason.
  static void record(std::string_view url, CachedImageSize size);

  /// Forgets one URL, as a caller does when a request is cancelled and the
  /// bytes it had are no longer anything the cache should vouch for.
  static void forget(std::string_view url);

  /// Forgets everything. For tests.
  static void clear();

  /// Entries held, for tests and for the benchmark's memory check.
  static size_t size();

  /// Builds the per-view answers a document needs, from this cache and the
  /// view's policy.
  ///
  /// This is what a shadow node calls at measurement, and it is why a recycled
  /// message is laid out at its final size on the first frame: the answers come
  /// from the cache rather than from whichever view happens to be mounted.
  ///
  /// `ordinals` is the document's image ordinals, in order. An image with no
  /// cache entry gets a `Loading` answer, which is the placeholder.
  static ImageStateLayout stateLayoutFor(const std::vector<uint32_t> &ordinals,
                                         const std::vector<std::string> &urls, const std::vector<bool> &loadable,
                                         bool enabled);
};

} // namespace downstream
