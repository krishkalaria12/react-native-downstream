#pragma once

#include "Caret.h"
#include "CodeLayout.h"
#include "Document.h"
#include "theme/Theme.h"

#include <cstdint>
#include <utility>
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
  /// Every code block in the document is laid out at the card's width, so its
  /// long lines wrap. This says nothing about a document that has a mix, which
  /// `CodeLayout.h`'s `resolveCodeBlock` decides one block at a time.
  kChromeCodeWrap = 1u << 2,
};

/// A width, font scale, and chrome a view was measured at, in points.
struct LayoutHint {
  float fontScale{1};
  float width{0};
  /// See `Chrome`. Height depends on it, so two layouts that differ only here
  /// are two layouts.
  uint32_t chrome{kChromeNone};
  /// The code layout this measurement is at: the props and the reader's choices
  /// together. Every input that changes a card's height travels in here, so a
  /// worker measures the layout the view is about to draw.
  ///
  /// These are the reader's own choices rather than a revision of them: a
  /// revision would separate two states that lay out identically (a block
  /// toggled on and back off), and the hint has to be enough on its own for the
  /// worker to measure the same heights the view draws. The choice sets are
  /// almost always empty, so the comparison below is one size check in practice.
  CodeLayout code;
  /// The caret to draw, resolved against the document. It is a height, like
  /// `chrome`, because the caret's placeholder is in the measured text: only the
  /// last segment changes, but a layout measured without it and a layout drawn
  /// with it would not agree about where the document ends.
  PlacedCaret caret;
  /// The theme this view draws with, or null for the default one. A worker
  /// builds the rendering the view will mount, so the theme has to be part of
  /// the hint: without it the worker would prepare an unthemed layout that
  /// Fabric measures again on the JavaScript thread.
  Theme::Handle theme;

  // Constructors rather than aggregate initialisation, so a caller that names
  // only a width and a scale says so instead of leaving the chrome, the code
  // layout and the theme to default, and a caller that names all of them cannot
  // leave one out by accident.
  LayoutHint() = default;
  LayoutHint(float scale, float at) : fontScale(scale), width(at) {}
  LayoutHint(float scale, float at, uint32_t chromeIn, CodeLayout codeIn, Theme::Handle themeIn = nullptr)
      : fontScale(scale), width(at), chrome(chromeIn), code(std::move(codeIn)), theme(std::move(themeIn)) {}
  LayoutHint(float scale, float at, uint32_t chromeIn, CodeLayout codeIn, PlacedCaret caretIn,
             Theme::Handle themeIn = nullptr)
      : fontScale(scale), width(at), chrome(chromeIn), code(std::move(codeIn)), caret(caretIn), theme(std::move(themeIn)) {}

  /// Equal when two layouts would draw the same sizes. The theme compares by
  /// id rather than by pointer, so two equal themes are one layout hint and a
  /// view that switched to a theme with the same values reuses every prepared
  /// rendering rather than rebuilding them.
  bool operator==(const LayoutHint &other) const {
    return fontScale == other.fontScale && width == other.width && chrome == other.chrome &&
        code == other.code && caret == other.caret && themeId() == other.themeId();
  }

  /// The theme's id, or 0 for the default one, which is its id too.
  uint64_t themeId() const { return theme ? theme->id() : 0; }
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
