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
                          std::vector<uint32_t> wrapToggles = {}, bool codeBlockWrap = false)
      : document(std::move(document)),
        fontSizeMultiplier(fontSizeMultiplier),
        chrome(chrome),
        wrapToggles(std::move(wrapToggles)),
        codeBlockWrap(codeBlockWrap) {}

  downstream::DocumentHandle document;
  Float fontSizeMultiplier{1.0};
  /// See `chromeForProps`. The resolved value travels with the state so the view
  /// lays out at the height the node measured.
  uint32_t chrome{downstream::kChromeNone};
  /// The code blocks whose layout differs from `codeBlockWrap`, keyed by the
  /// block's ordinal among the document's code blocks, sorted. Ordinals are
  /// stable across append-only snapshots, so a choice survives streaming.
  ///
  /// The set is the reader's own state rather than a revision of it, because
  /// the platforms need the decisions themselves, not a number that says they
  /// changed. `chrome` cannot stand in for it: it says every code block wraps
  /// or that not all of them do, and a document with a mix needs the set.
  std::vector<uint32_t> wrapToggles;
  /// The `codeBlockWrap` prop, so the two halves of the decision travel
  /// together and a platform never has to infer one from the other.
  bool codeBlockWrap{false};

  /// True when the code block at `ordinal` is laid out at the card's width
  /// rather than at its own length.
  bool isWrapped(uint32_t ordinal) const {
    return downstream::codeWraps(codeBlockWrap, wrapToggles, ordinal);
  }

  /// A copy with `ordinal` toggled and its chrome resolved again.
  DownstreamMarkdownState toggled(uint32_t ordinal, uint32_t controls) const {
    DownstreamMarkdownState next(*this);
    const auto entry = std::find(next.wrapToggles.begin(), next.wrapToggles.end(), ordinal);
    if (entry != next.wrapToggles.end()) {
      next.wrapToggles.erase(entry);
    } else {
      next.wrapToggles.push_back(ordinal);
      // Sorted, so two states with the same set compare the same however they
      // were reached, and so `codeWraps` can binary-search it.
      std::sort(next.wrapToggles.begin(), next.wrapToggles.end());
    }
    next.chrome = chromeForProps(controls, next.codeBlockWrap, next.wrapToggles);
    return next;
  }

  /// A copy with the same choices and a chrome resolved for different props,
  /// which is what a change to `controls` or `codeBlockWrap` needs.
  DownstreamMarkdownState rechromed(uint32_t controls, bool wrap) const {
    DownstreamMarkdownState next(*this);
    next.codeBlockWrap = wrap;
    next.chrome = chromeForProps(controls, wrap, next.wrapToggles);
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
        codeBlockWrap(previous.codeBlockWrap) {
    if (!data.isObject()) {
      return;
    }
    if (auto value = data.get_ptr("chrome")) {
      chrome = static_cast<uint32_t>(value->asDouble());
    }
    if (auto value = data.get_ptr("codeBlockWrap")) {
      codeBlockWrap = value->asDouble() != 0;
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

  /// Java resolves the document through the native registry by id rather than
  /// copying the render model across JNI on every update.
  folly::dynamic getDynamic() const {
    downstream::DocumentRegistry::add(document);
    auto array = folly::dynamic::array();
    for (const uint32_t ordinal : wrapToggles) {
      array.push_back(static_cast<double>(ordinal));
    }
    return folly::dynamic::object("documentId", document ? static_cast<double>(document->id) : 0.0)(
        "fontScale", static_cast<double>(fontSizeMultiplier))("chrome", static_cast<double>(chrome))(
        // A number rather than a bool, so the state crosses JNI as one type.
        "codeBlockWrap", codeBlockWrap ? 1.0 : 0.0)("wrapToggles", std::move(array));
  }
#endif

};

} // namespace facebook::react
