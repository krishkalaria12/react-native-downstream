package com.downstream

import android.graphics.Rect
import android.os.Build
import android.os.Bundle
import android.view.View
import android.view.accessibility.AccessibilityEvent
import android.view.accessibility.AccessibilityNodeInfo
import android.view.accessibility.AccessibilityNodeProvider

/** Virtual reading stops for blocks and table rows inside one native view. */
internal class MarkdownAccessibility(
  private val host: View,
  private val entryCount: () -> Int,
  private val entryAt: (Int) -> Entry?,
) : AccessibilityNodeProvider() {
  internal class Entry(
    val text: CharSequence,
    val bounds: Rect,
    val heading: Boolean = false,
    val actions: List<Action> = emptyList(),
  )

  internal class Action(val label: String, val perform: () -> Unit)

  private var focused = -1

  override fun createAccessibilityNodeInfo(virtualViewId: Int): AccessibilityNodeInfo? {
    if (virtualViewId == View.NO_ID) {
      return AccessibilityNodeInfo.obtain(host).apply {
        host.onInitializeAccessibilityNodeInfo(this)
        for (index in 0 until entryCount()) addChild(host, index)
      }
    }
    val entry = entryAt(virtualViewId) ?: return null
    val bounds = Rect(entry.bounds)
    val location = IntArray(2)
    host.getLocationOnScreen(location)
    val screenBounds = Rect(bounds).apply { offset(location[0], location[1]) }
    return AccessibilityNodeInfo.obtain(host, virtualViewId).apply {
      packageName = host.context.packageName
      className = "android.widget.TextView"
      text = entry.text
      if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
        isHeading = entry.heading
      } else if (entry.heading) {
        collectionItemInfo = AccessibilityNodeInfo.CollectionItemInfo.obtain(0, 1, 0, 1, true)
      }
      isFocusable = true
      isVisibleToUser = Rect.intersects(bounds, Rect(0, 0, host.width, host.height))
      isAccessibilityFocused = focused == virtualViewId
      setParent(host)
      setBoundsInParent(bounds)
      setBoundsInScreen(screenBounds)
      addAction(if (focused == virtualViewId) AccessibilityNodeInfo.ACTION_CLEAR_ACCESSIBILITY_FOCUS
        else AccessibilityNodeInfo.ACTION_ACCESSIBILITY_FOCUS)
      entry.actions.forEachIndexed { index, action ->
        addAction(AccessibilityNodeInfo.AccessibilityAction(CUSTOM_ACTION_BASE + index, action.label))
      }
    }
  }

  override fun findFocus(focus: Int): AccessibilityNodeInfo? =
    if (focus == AccessibilityNodeInfo.FOCUS_ACCESSIBILITY && focused >= 0) createAccessibilityNodeInfo(focused) else null

  override fun performAction(virtualViewId: Int, action: Int, arguments: Bundle?): Boolean {
    val entry = entryAt(virtualViewId) ?: return false
    when (action) {
      AccessibilityNodeInfo.ACTION_ACCESSIBILITY_FOCUS -> {
        if (focused == virtualViewId) return false
        focused = virtualViewId
        sendEvent(virtualViewId, AccessibilityEvent.TYPE_VIEW_ACCESSIBILITY_FOCUSED, entry.text)
        return true
      }
      AccessibilityNodeInfo.ACTION_CLEAR_ACCESSIBILITY_FOCUS -> {
        if (focused != virtualViewId) return false
        focused = -1
        sendEvent(virtualViewId, AccessibilityEvent.TYPE_VIEW_ACCESSIBILITY_FOCUS_CLEARED, entry.text)
        return true
      }
    }
    val custom = action - CUSTOM_ACTION_BASE
    val callback = entry.actions.getOrNull(custom) ?: return false
    callback.perform()
    return true
  }

  fun contentChanged(resetFocus: Boolean = false) {
    if (resetFocus || focused >= entryCount()) focused = -1
    host.sendAccessibilityEvent(AccessibilityEvent.TYPE_WINDOW_CONTENT_CHANGED)
  }

  private fun sendEvent(id: Int, type: Int, text: CharSequence) {
    val event = AccessibilityEvent.obtain(type)
    event.packageName = host.context.packageName
    event.className = "android.widget.TextView"
    event.text.add(text)
    event.setSource(host, id)
    host.parent?.requestSendAccessibilityEvent(host, event)
  }

  companion object {
    private const val CUSTOM_ACTION_BASE = 0x01000000
  }
}
