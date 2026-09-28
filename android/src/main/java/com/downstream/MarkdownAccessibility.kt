package com.downstream

import android.graphics.Rect
import android.os.Build
import android.os.Bundle
import android.view.View
import android.view.accessibility.AccessibilityEvent
import android.view.accessibility.AccessibilityNodeInfo
import android.view.accessibility.AccessibilityNodeProvider
import androidx.core.view.accessibility.AccessibilityNodeInfoCompat

internal class AccessibleTextPiece(val start: Int, val end: Int, val linkIndex: Int? = null)

/** Split visible text around links without repeating link labels in TalkBack. */
internal fun linkPieces(text: android.text.Spanned, start: Int, end: Int): List<AccessibleTextPiece> {
  val pieces = ArrayList<AccessibleTextPiece>()
  var cursor = start
  val links = text.getSpans(start, end, LinkSpan::class.java).sortedBy { text.getSpanStart(it) }
  for (link in links) {
    val linkStart = text.getSpanStart(link).coerceIn(cursor, end)
    val linkEnd = text.getSpanEnd(link).coerceIn(linkStart, end)
    if (cursor < linkStart) pieces.add(AccessibleTextPiece(cursor, linkStart))
    if (linkStart < linkEnd) pieces.add(AccessibleTextPiece(linkStart, linkEnd, link.index))
    cursor = linkEnd
  }
  if (cursor < end || pieces.isEmpty()) pieces.add(AccessibleTextPiece(cursor, end))
  return pieces
}

/** Virtual reading stops for text and links inside one native view. */
internal class MarkdownAccessibility(
  private val host: View,
  private val entryCount: () -> Int,
  private val entryAt: (Int) -> Entry?,
) : AccessibilityNodeProvider() {
  internal class Entry(
    val text: CharSequence,
    val bounds: Rect,
    val heading: Boolean = false,
    val linkPress: (() -> Unit)? = null,
  )

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
      if (entry.linkPress != null) {
        isClickable = true
        addAction(AccessibilityNodeInfo.ACTION_CLICK)
        AccessibilityNodeInfoCompat.wrap(this).roleDescription =
          host.context.getString(com.facebook.react.R.string.link_description)
      }
      isVisibleToUser = Rect.intersects(bounds, Rect(0, 0, host.width, host.height))
      isAccessibilityFocused = focused == virtualViewId
      setParent(host)
      setBoundsInParent(bounds)
      setBoundsInScreen(screenBounds)
      addAction(if (focused == virtualViewId) AccessibilityNodeInfo.ACTION_CLEAR_ACCESSIBILITY_FOCUS
        else AccessibilityNodeInfo.ACTION_ACCESSIBILITY_FOCUS)
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
      AccessibilityNodeInfo.ACTION_CLICK -> {
        val press = entry.linkPress ?: return false
        press()
        return true
      }
    }
    return false
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
}
