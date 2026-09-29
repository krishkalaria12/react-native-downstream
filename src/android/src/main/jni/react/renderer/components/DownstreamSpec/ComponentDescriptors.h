#pragma once

// Shadows the Codegen header so autolinking registers the hand-written
// descriptor from src/cpp/fabric.

#include "DownstreamMarkdownComponentDescriptor.h"

#include <react/renderer/componentregistry/ComponentDescriptorProviderRegistry.h>

namespace facebook::react {

void DownstreamSpec_registerComponentDescriptorsFromCodegen(
    std::shared_ptr<const ComponentDescriptorProviderRegistry> registry);

} // namespace facebook::react
