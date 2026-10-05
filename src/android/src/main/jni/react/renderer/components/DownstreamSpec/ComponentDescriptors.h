#pragma once

// Shadows the Codegen header so autolinking registers the hand-written
// descriptor from src/cpp/fabric. The fence slot has no custom shadow node, so
// its descriptor is the one Codegen would have declared here.

#include "DownstreamMarkdownComponentDescriptor.h"

#include <react/renderer/components/DownstreamSpec/ShadowNodes.h>
#include <react/renderer/componentregistry/ComponentDescriptorProviderRegistry.h>
#include <react/renderer/core/ConcreteComponentDescriptor.h>

namespace facebook::react {

using DownstreamFenceSlotComponentDescriptor = ConcreteComponentDescriptor<DownstreamFenceSlotShadowNode>;

void DownstreamSpec_registerComponentDescriptorsFromCodegen(
    std::shared_ptr<const ComponentDescriptorProviderRegistry> registry);

} // namespace facebook::react
