#pragma once

#include "Controls.h"
#include "Document.h"
#include "DocumentRegistry.h"
#include "RenderPreparer.h"

#include <react/renderer/graphics/Float.h>

#include <algorithm>
#include <cstdint>
#include <vector>

#ifdef RN_SERIALIZABLE_STATE
#include <folly/dynamic.h>
#endif

namespace facebook::react {

/// The chrome a document lays out with, from the props and the reader's
/// choices. Shared with the platforms so a measurement and a layout cannot
/// disagree about a height.
using downstream::chromeForProps;

/// The bits of the `controls` prop, so this layer reads `ControlBits::CodeCopy`
/// next to the rest of the Fabric code.
namespace ControlBits = downstream::ControlBits;

/// Mounting state: the parsed document, the font scale it was measured with,
/// and the reader's per-block wrap choices. The view renders exactly this, so a
/// mounted frame never mixes content from one revision with a size from another.
class DownstreamMarkdownState final {
public:
  DownstreamMarkdownState() = default;

  DownstreamMarkdownState(downstream::DocumentHandle document, Float fontSizeMultiplier, uint32_t chrome = 0,
                          std::vector<uint32_t> wrapToggles = {}, uint32_t wrapRevision = 0)
      : document(std::move(document)),
        fontSizeMultiplier(fontSizeMultiplier),
        chrome(chrome),
        wrapToggles(std::move(wrapToggles)),
        wrapRevision(wrapRevision) {}

  downstream::DocumentHandle document;
  Float fontSizeMultiplier{1.0};
  /// See `chromeForProps`. The resolved value travels with the state so the view
  /// lays out at the height the node measured.
  uint32_t chrome{downstream::kChromeNone};
  /// The code blocks whose layout differs from `codeBlockWrap`, keyed by the
  /// block's ordinal among the document's code blocks. Ordinals are stable
  /// across append-only snapshots, so a choice survives streaming.
  std::vector<uint32_t> wrapToggles;
  /// A revision of the toggle set, which is what the layout key carries. A
  /// revision moves whenever the set does, so the key stays a small integer a
  /// measurement compares in one instruction.
  uint32_t wrapRevision{0};

  /// True when the code block at `ordinal` is laid out at the card's width
  /// rather than at its own length.
  bool isWrapped(uint32_t ordinal, bool codeBlockWrap) const {
    const bool toggled =
        std::find(wrapToggles.begin(), wrapToggles.end(), ordinal) != wrapToggles.end();
    return toggled != codeBlockWrap;
  }

  /// A copy with `ordinal` toggled, its chrome resolved again, and the revision
  /// moved on so every measurement cache misses once.
  DownstreamMarkdownState toggled(uint32_t ordinal, uint32_t controls, bool codeBlockWrap) const {
    DownstreamMarkdownState next(*this);
    const auto entry = std::find(next.wrapToggles.begin(), next.wrapToggles.end(), ordinal);
    if (entry != next.wrapToggles.end()) {
      next.wrapToggles.erase(entry);
    } else {
      next.wrapToggles.push_back(ordinal);
      // Sorted, so two states with the same set compare the same however they
      // were reached.
      std::sort(next.wrapToggles.begin(), next.wrapToggles.end());
    }
    next.wrapRevision = wrapRevision + 1;
    next.chrome = chromeForProps(controls, codeBlockWrap, next.wrapToggles);
    return next;
  }

  /// A copy with the same choices and a chrome resolved for different props,
  /// which is what a change to `controls` or `codeBlockWrap` needs.
  DownstreamMarkdownState rechromed(uint32_t controls, bool codeBlockWrap) const {
    DownstreamMarkdownState next(*this);
    next.chrome = chromeForProps(controls, codeBlockWrap, next.wrapToggles);
    return next;
  }

#ifdef RN_SERIALIZABLE_STATE
  /// The view hands a new state back through StateWrapper, so the choices the
  /// reader just made have to survive the round trip through JNI.
  DownstreamMarkdownState(const DownstreamMarkdownState &previous, folly::dynamic data)
      : document(previous.document),
        fontSizeMultiplier(previous.fontSizeMultiplier),
        chrome(previous.chrome),
        wrapToggles(previous.wrapToggles),
        wrapRevision(previous.wrapRevision) {
    if (!data.isObject()) {
      return;
    }
    if (auto value = data.get_ptr("chrome")) {
      chrome = static_cast<uint32_t>(value->asDouble());
    }
    if (auto value = data.get_ptr("wrapRevision")) {
      wrapRevision = static_cast<uint32_t>(value->asDouble());
    }
    if (auto value = data.get_ptr("wrapToggles")) {
      if (value->isArray()) {
        std::vector<uint32_t> toggles;
        toggles.reserve(value->size());
        for (const auto &entry : *value) {
          toggles.push_back(static_cast<uint32_t>(entry.asDouble()));
        }
        wrapToggles = std::move(toggles);
      }
    }
  }

  /// Java resolves the document through the native registry by id rather
  /// than copying the render model across JNI on every update.
  folly::dynamic getDynamic() const {
    downstream::DocumentRegistry::add(document);
    auto array = folly::dynamic::array();
    for (const uint32_t ordinal : wrapToggles) {
      array.push_back(static_cast<double>(ordinal));
    }
    return folly::dynamic::object("documentId", document ? static_cast<double>(document->id) : 0.0)(
        "fontScale", static_cast<double>(fontSizeMultiplier))("chrome", static_cast<double>(chrome))(
        "wrapToggles", std::move(array))("wrapRevision", static_cast<double>(wrapRevision));
  }
#endif
};

} // namespace facebook::react
