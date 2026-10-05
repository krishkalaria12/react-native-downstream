#pragma once

// The sizes a host view's React children take, as a measurement input and as
// mounting state.
//
// A fenced block whose language the app registered is drawn by a React child
// the native view mounts as a real Fabric child, rather than by the code card.
// The child has no size of its own until it has been measured, so its height
// is an input to measuring the document, exactly as a loaded image's box is:
// the platform needs every height at once to lay the document out, and the
// child needs the column it will be drawn in. `HostLayout` is that input, and
// it lives in the state beside `ImageStateLayout` so a mounted frame never
// mixes a child's height from one measurement with a document from another.
//
// Empty means no child asked to be hosted, which is the case for every view
// that registers no renderers: the same key, the same measurement, the same
// drawing.

#include <algorithm>
#include <cstdint>
#include <vector>

namespace downstream {

/// One hosted block's measured height, in points.
struct HostEntry {
  /// The code block's ordinal, which is how the React slot is keyed and how
  /// the state and the shadow tree name the same block.
  uint32_t ordinal{0};
  float height{0};

  bool operator==(const HostEntry &) const = default;
};

/// Every React child the view mounts into a fence, and the height it measured
/// at. Sorted by ordinal, so a caller binary-searches and the state round trip
/// through JNI reads back in one order.
struct HostLayout {
  std::vector<HostEntry> entries;

  bool operator==(const HostLayout &) const = default;

  bool empty() const { return entries.empty(); }

  /// The height for `ordinal`, or 0 when the view hosts no child for it.
  float heightFor(uint32_t ordinal) const {
    const auto it = std::lower_bound(entries.begin(), entries.end(), ordinal,
                                     [](const HostEntry &entry, uint32_t value) { return entry.ordinal < value; });
    return it != entries.end() && it->ordinal == ordinal ? it->height : 0;
  }

  /// True when `ordinal` has an entry, which is also how a platform knows the
  /// code block is drawn as a slot rather than as a card.
  ///
  /// Presence, not height: a component that renders nothing has a height of
  /// zero and still owns its fence. The card is gone because the app said so,
  /// not because the app drew something, and a renderer that returns null for a
  /// fence the reader can see the source of is a deliberate choice rather than a
  /// mistake to paper over with a code card.
  bool hosts(uint32_t ordinal) const {
    const auto it = std::lower_bound(entries.begin(), entries.end(), ordinal,
                                     [](const HostEntry &entry, uint32_t value) { return entry.ordinal < value; });
    return it != entries.end() && it->ordinal == ordinal;
  }
};

} // namespace downstream
