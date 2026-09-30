#pragma once

#include "Document.h"

#include <cstdint>
#include <vector>

namespace downstream {

/// Which control chrome a layout has: a code header, a table toolbar, and code
/// that wraps rather than scrolls sideways. These are heights, not renderings,
/// so they are part of a layout key rather than of the text a segment caches.
enum Chrome : uint32_t {
  kChromeNone = 0,
  /// A header row above each code block.
  kChromeCodeHeader = 1u << 0,
  /// A toolbar row above each table.
  kChromeTableToolbar = 1u << 1,
  /// Code laid out at the card's width, so long lines wrap.
  kChromeCodeWrap = 1u << 2,
};

/// A width, font scale, and chrome a view was measured at, in points.
struct LayoutHint {
  float fontScale{1};
  float width{0};
  /// See `Chrome`. Height depends on it, so two layouts that differ only here
  /// are two layouts.
  uint32_t chrome{kChromeNone};
  /// A revision of the reader's wrap choices. The choices themselves are not in
  /// the key: a revision moves whenever they do, which keeps the key a small
  /// integer that a cached measurement compares in one instruction.
  uint32_t revision{0};

  bool operator==(const LayoutHint &other) const {
    return fontScale == other.fontScale && width == other.width && chrome == other.chrome &&
        revision == other.revision;
  }
};

/// Moves platform text work off the JavaScript and UI threads. After a worker
/// parses a document, it builds the platform rendering (attributed strings on
/// iOS, spans and precomputed text on Android) and measures it at the widths
/// views recently used. Fabric's measurement and the view's mount then find
/// both in the platform caches instead of doing the work themselves.
class RenderPreparer {
public:
  /// Builds and measures one document for each hint. Called on worker
  /// threads, possibly for several documents at once.
  using Prepare = void (*)(const DocumentHandle &document, const std::vector<LayoutHint> &hints);

  /// Set once by the platform layer. Without it, preparing does nothing and
  /// the platform builds the rendering when Fabric first measures.
  static void install(Prepare prepare);

  static void prepare(const DocumentHandle &document, const std::vector<LayoutHint> &hints);

  /// Records a measurement, so later documents are prepared at its width.
  static void noteLayout(LayoutHint hint);
  /// The most recent distinct measurements, newest first.
  static std::vector<LayoutHint> recentLayouts();
};

} // namespace downstream
