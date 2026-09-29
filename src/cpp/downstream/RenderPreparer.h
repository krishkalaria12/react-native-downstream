#pragma once

#include "Document.h"

#include <vector>

namespace downstream {

/// A width and font scale that a view was measured at, in points.
struct LayoutHint {
  float fontScale{1};
  float width{0};

  bool operator==(const LayoutHint &other) const {
    return fontScale == other.fontScale && width == other.width;
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
