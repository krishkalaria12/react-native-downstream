#pragma once

#include "DownstreamMarkdownShadowNode.h"

#include <react/renderer/core/ConcreteComponentDescriptor.h>

namespace facebook::react {

class DownstreamMarkdownViewComponentDescriptor final
    : public ConcreteComponentDescriptor<DownstreamMarkdownShadowNode> {
public:
  explicit DownstreamMarkdownViewComponentDescriptor(const ComponentDescriptorParameters &parameters)
      : ConcreteComponentDescriptor(parameters),
        layoutManager_(std::make_shared<DownstreamTextLayoutManager>(contextContainer_)) {}

  void adopt(ShadowNode &shadowNode) const override {
    ConcreteComponentDescriptor::adopt(shadowNode);
    static_cast<DownstreamMarkdownShadowNode &>(shadowNode).setLayoutManager(layoutManager_);
  }

private:
  const std::shared_ptr<const DownstreamTextLayoutManager> layoutManager_;
};

} // namespace facebook::react
