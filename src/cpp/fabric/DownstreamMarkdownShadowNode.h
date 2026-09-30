#pragma once

#include "DownstreamMarkdownState.h"
#include "DownstreamTextLayoutManager.h"
#include "PreparedDocument.h"

#include <react/renderer/components/DownstreamSpec/EventEmitters.h>
#include <react/renderer/components/DownstreamSpec/Props.h>
#include <react/renderer/components/view/ConcreteViewShadowNode.h>

#include <atomic>
#include <memory>
#include <optional>
#include <string>

namespace facebook::react {

JSI_EXPORT extern const char DownstreamMarkdownViewComponentName[];

/// Measures a document with the platform text system. The parsed document
/// reaches the view through state, so the view never parses.
///
/// A controlled `markdown` value is parsed and prepared on the worker pool as
/// soon as the node receives it, so every Markdown view in a commit is
/// prepared in parallel while React finishes the commit. Layout collects the
/// result. A large source is not waited for: the view keeps its previous
/// document until the preparation finishes and a state update relayouts it.
class DownstreamMarkdownShadowNode final
    : public ConcreteViewShadowNode<DownstreamMarkdownViewComponentName, DownstreamMarkdownViewProps,
                                    DownstreamMarkdownViewEventEmitter, DownstreamMarkdownState> {
public:
  DownstreamMarkdownShadowNode(const ShadowNodeFragment &fragment, const ShadowNodeFamily::Shared &family,
                               ShadowNodeTraits traits);
  DownstreamMarkdownShadowNode(const ShadowNode &sourceShadowNode, const ShadowNodeFragment &fragment);

  static ShadowNodeTraits BaseTraits() {
    auto traits = ConcreteViewShadowNode::BaseTraits();
    traits.set(ShadowNodeTraits::Trait::LeafYogaNode);
    traits.set(ShadowNodeTraits::Trait::MeasurableYogaNode);
    return traits;
  }

  void setLayoutManager(std::shared_ptr<const DownstreamTextLayoutManager> layoutManager);

  Size measureContent(const LayoutContext &layoutContext, const LayoutConstraints &layoutConstraints) const override;

  void layout(LayoutContext layoutContext) override;

private:
  struct PendingError {
    std::string code;
    std::string message;
  };

  /// One controlled source's preparation, shared by every clone of the node
  /// that shows that source.
  struct ControlledSource {
    std::shared_ptr<downstream::PreparedDocument> prepared;
    /// Set once a relayout has been requested for when the preparation ends.
    std::atomic<bool> relayoutRequested{false};
  };

  void requestControlledSource();
  /// Returns the document to measure and mount. A failed parse keeps the last
  /// valid document.
  const downstream::DocumentHandle &getDocument() const;
  /// Asks for a relayout once a preparation that layout did not wait for ends.
  void relayoutWhenPrepared() const;

  /// The chrome a document lays out with, from the props and the reader's
  /// choices. It is a height, not a rendering, so it is recomputed here rather
  /// than baked into a segment's text.
  uint32_t chromeFor(const DownstreamMarkdownState &state) const;

  std::shared_ptr<const DownstreamTextLayoutManager> layoutManager_;
  std::shared_ptr<ControlledSource> controlled_;
  mutable downstream::DocumentHandle document_;
  /// The revision has not chosen its document yet. Once chosen, measurement
  /// and mounting use the same one.
  mutable bool needsDocument_{true};
  /// The revision chose a fallback because its source was still being
  /// prepared; the next revision collects the result.
  mutable bool awaitingPrepared_{false};
  mutable std::optional<PendingError> pendingError_;
};

} // namespace facebook::react
