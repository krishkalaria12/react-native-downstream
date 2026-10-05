package com.downstream

import android.view.View
import com.facebook.react.uimanager.SimpleViewManager
import com.facebook.react.uimanager.ThemedReactContext
import com.facebook.react.uimanager.ViewManagerDelegate
import com.facebook.react.viewmanagers.DownstreamFenceSlotManagerDelegate
import com.facebook.react.viewmanagers.DownstreamFenceSlotManagerInterface

/**
 * The view manager for <DownstreamFenceSlot>, the child one hosted fence
 * lives in.
 *
 * A plain view: the slot carries only the code block's ordinal, which the shadow
 * node reads to measure the child at that block's column, and everything a
 * renderer draws is the app's own component inside it. It is never flattened and
 * never touched here — `DownstreamMarkdownViewManager` mounts it, and the
 * Markdown view's shadow node measures it, places it, and hides it when its block
 * has gone.
 */
class FenceSlotViewManager :
  SimpleViewManager<View>(),
  DownstreamFenceSlotManagerInterface<View> {
  private val delegate = DownstreamFenceSlotManagerDelegate(this)

  override fun getDelegate(): ViewManagerDelegate<View> = delegate

  override fun getName() = NAME

  override fun createViewInstance(context: ThemedReactContext): View = View(context)

  override fun setOrdinal(view: View, value: Int) {
    // The ordinal is read by the shadow node, from the props, and never by the
    // view: the frame the slot is given already says which block it is for, and
    // two slots cannot claim the same block.
  }

  companion object {
    const val NAME = "DownstreamFenceSlot"
  }
}
