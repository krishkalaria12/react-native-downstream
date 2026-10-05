#import "DownstreamFenceSlotView.h"

#import <react/renderer/components/DownstreamSpec/ComponentDescriptors.h>

using namespace facebook::react;

@implementation DownstreamFenceSlotView

/// The slot's own descriptor rather than `RCTView`'s. Fabric registers a
/// component under the name its descriptor reports, so a class that hands back
/// the `View` descriptor leaves `DownstreamFenceSlot` unregistered: the slot then
/// mounts as an unimplemented view whose shadow node the Markdown view cannot
/// recognise, and no fence is ever hosted.
+ (ComponentDescriptorProvider)componentDescriptorProvider {
  return concreteComponentDescriptorProvider<DownstreamFenceSlotComponentDescriptor>();
}

@end
