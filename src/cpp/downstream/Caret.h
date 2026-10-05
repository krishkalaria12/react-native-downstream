#pragma once

// Where a caret goes while a stream is still arriving, and whether it shows at
// all.
//
// The caret is the one part of this feature that changes the rendering rather
// than the drawing: it reserves space, so its height has to be in the measured
// layout and in both platforms' rendering cache keys. Deciding that here rather
// than in each view is what keeps the two platforms from disagreeing about a
// caret that sits after a table in one and inline in the other.

#include "Document.h"

#include <cstdint>

namespace downstream {

/// The shape drawn after the newest text. Both are drawn rather than typeset, so
/// they look the same in every font and cannot change a line's height.
enum class CaretKind : uint8_t {
  None = 0,
  /// A rounded bar, the proportions of U+258B.
  Block = 1,
  /// A filled dot, the proportions of U+25CF.
  Circle = 2,
};

/// Whether text is still arriving, from the point of view of a view that may or
/// may not know.
enum class Animating : uint8_t {
  /// Follow the document's own `streaming` flag, which a session sets on every
  /// snapshot before it finishes.
  Auto = 0,
  On = 1,
  Off = 2,
};

/// Everything that decides whether a caret is drawn. Part of a layout key, so it
/// compares by value.
struct CaretSpec {
  CaretKind kind{CaretKind::None};
  Animating animating{Animating::Off};

  bool operator==(const CaretSpec &other) const = default;
};

enum class CaretPlace : uint8_t {
  /// No caret.
  None = 0,
  /// After the last character of the last block: prose, or a fence still open,
  /// which is where the next character of code will appear.
  AfterText = 1,
  /// A line of its own, after the last segment, or alone in an empty document.
  /// This is where the next block's first line will start, so when text arrives
  /// the caret's line is replaced without a jump.
  OwnLine = 2,
};

/// Where the caret goes, or that there is none.
///
/// A caret only draws when `kind` is not `None` and either `animating` is `On` or
/// it is `Auto` while the document is still streaming. A controlled document
/// leaves `streaming` false, which is what makes the caret a controlled view's
/// own decision to turn on.
CaretPlace placeCaret(const Document &document, const CaretSpec &spec);

/// A caret resolved against one document: its shape and where it goes.
///
/// This rather than `CaretSpec` is what a layout key and a rendering cache key
/// carry, because both have to separate an inline caret from one on a line of
/// its own, and two props can place the same shape differently.
struct PlacedCaret {
  CaretKind kind{CaretKind::None};
  CaretPlace place{CaretPlace::None};

  bool any() const { return kind != CaretKind::None && place != CaretPlace::None; }
  bool operator==(const PlacedCaret &other) const = default;
};

/// `spec` decided against `document`.
///
/// A caret with no place is no caret at all, shape included, so a cache key has
/// one spelling for "none" and a platform that reads only the shape cannot draw a
/// caret the engine did not place.
inline PlacedCaret resolveCaret(const Document &document, const CaretSpec &spec) {
  const CaretPlace place = placeCaret(document, spec);
  return place == CaretPlace::None ? PlacedCaret{} : PlacedCaret{spec.kind, place};
}

} // namespace downstream