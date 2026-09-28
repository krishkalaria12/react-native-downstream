#pragma once

#include "DownstreamMarkdownState.h"
#include "DownstreamTextLayoutManager.h"

#include <react/renderer/components/DownstreamSpec/EventEmitters.h>
#include <react/renderer/components/DownstreamSpec/Props.h>
#include <react/renderer/components/view/ConcreteViewShadowNode.h>

#include <optional>
#include <string>

namespace facebook::react {

JSI_EXPORT extern const char DownstreamMarkdownViewComponentName[];

/// Parses the controlled `markdown` prop once per value and measures the
/// result with the platform text system. The parsed document reaches the view
/// through state, so the view never parses.
class DownstreamMarkdownShadowNode final
    : public ConcreteViewShadowNode<DownstreamMarkdownViewComponentName, DownstreamMarkdownViewProps,
                                    DownstreamMarkdownViewEventEmitter, DownstreamMarkdownState> {
public:
  using ConcreteViewShadowNode::ConcreteViewShadowNode;

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

  /// Parses on first use. A failed parse keeps the last valid document.
  const downstream::DocumentHandle &getDocument() const;

  std::shared_ptr<const DownstreamTextLayoutManager> layoutManager_;
  mutable downstream::DocumentHandle document_;
  mutable bool needsParse_{true};
  mutable std::optional<PendingError> pendingError_;
};

} // namespace facebook::react
