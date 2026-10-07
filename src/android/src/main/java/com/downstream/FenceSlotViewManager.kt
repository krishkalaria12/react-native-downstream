package com.downstream

import com.facebook.react.uimanager.ThemedReactContext
import com.facebook.react.uimanager.ViewGroupManager
import com.facebook.react.uimanager.ViewManagerDelegate
import com.facebook.react.viewmanagers.DownstreamFenceSlotManagerDelegate
import com.facebook.react.viewmanagers.DownstreamFenceSlotManagerInterface
import com.facebook.react.views.view.ReactViewGroup

/**
 * The view manager for <DownstreamFenceSlot>, the child one hosted fence
 * lives in.
 *
 * The view React's own `<View>` is: the slot's whole job is to hold the app's
 * component, so it has to be a view group that Fabric can mount children into,
 * and `ReactViewGroup` is the one whose children Fabric already knows how to
 * place. The slot carries only the code block's ordinal, which the shadow node
 * reads to measure the child at that block's column. It is never flattened and
 * never touched here — `DownstreamMarkdownViewManager` mounts it, and the
 * Markdown view's shadow node measures it, places it, and hides it when its block
 * has gone.
 */
class FenceSlotViewManager :
  ViewGroupManager<ReactViewGroup>(),
  DownstreamFenceSlotManagerInterface<ReactViewGroup> {
  private val delegate = DownstreamFenceSlotManagerDelegate(this)

  override fun getDelegate(): ViewManagerDelegate<ReactViewGroup> = delegate

  override fun getName() = NAME

  override fun createViewInstance(context: ThemedReactContext): ReactViewGroup = ReactViewGroup(context)

  override fun setOrdinal(view: ReactViewGroup, value: Int) {
    // The ordinal is read by the shadow node, from the props, and never by the
    // view: the frame the slot is given already says which block it is for, and
    // two slots cannot claim the same block.
  }

  override fun setContainerId(view: ReactViewGroup, value: Int) {
    // Like `ordinal`: the shadow node reads it, the view never does. A tag slot
    // carries its container's id here and -1 in `ordinal`, so the two key
    // spaces cannot claim each other's blocks.
  }

  companion object {
    const val NAME = "DownstreamFenceSlot"
  }
}
