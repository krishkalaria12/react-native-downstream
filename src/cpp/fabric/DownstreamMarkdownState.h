#pragma once

#include "Document.h"
#include "DocumentRegistry.h"

#include <react/renderer/graphics/Float.h>

#ifdef RN_SERIALIZABLE_STATE
#include <folly/dynamic.h>
#endif

namespace facebook::react {

/// Mounting state: the parsed document and the font scale it was measured
/// with. The view renders exactly this pair, so a mounted frame never mixes
/// content from one revision with a size from another.
class DownstreamMarkdownState final {
public:
  DownstreamMarkdownState() = default;

  DownstreamMarkdownState(downstream::DocumentHandle document, Float fontSizeMultiplier)
      : document(std::move(document)), fontSizeMultiplier(fontSizeMultiplier) {}

  downstream::DocumentHandle document;
  Float fontSizeMultiplier{1.0};

#ifdef RN_SERIALIZABLE_STATE
  /// Native never updates this state; Java only reads it.
  DownstreamMarkdownState(const DownstreamMarkdownState &previous, folly::dynamic /*data*/)
      : DownstreamMarkdownState(previous) {}

  /// Java resolves the document through the native registry by id rather
  /// than copying the render model across JNI on every update.
  folly::dynamic getDynamic() const {
    downstream::DocumentRegistry::add(document);
    return folly::dynamic::object("documentId", document ? static_cast<double>(document->id) : 0.0)(
        "fontScale", static_cast<double>(fontSizeMultiplier));
  }
#endif
};

} // namespace facebook::react
