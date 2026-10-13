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
  val linkRanges = ArrayList<AccessibleTextPiece>()
  val spans = text.getSpans(start, end, LinkSpan::class.java).sortedWith(
    compareBy<LinkSpan> { text.getSpanStart(it) }.thenByDescending { text.getSpanEnd(it) },
  )
  for (span in spans) {
    val spanStart = text.getSpanStart(span).coerceIn(start, end)
    val spanEnd = text.getSpanEnd(span).coerceIn(spanStart, end)
    val previous = linkRanges.lastOrNull()
    if (previous != null && previous.linkIndex == span.index && spanStart <= previous.end) {
      linkRanges[linkRanges.lastIndex] = AccessibleTextPiece(previous.start, maxOf(previous.end, spanEnd), span.index)
    } else {
      // Link spans cannot overlap in Markdown. If one does, keep the earlier
      // occurrence's text and expose only the still-uncovered suffix.
      val uncoveredStart = maxOf(spanStart, previous?.end ?: start)
      if (uncoveredStart < spanEnd) linkRanges.add(AccessibleTextPiece(uncoveredStart, spanEnd, span.index))
    }
  }
  val pieces = ArrayList<AccessibleTextPiece>()
  var cursor = start
  for (link in linkRanges) {
    if (cursor < link.start) pieces.add(AccessibleTextPiece(cursor, link.start))
    pieces.add(link)
    cursor = link.end
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
    /**
     * A press that does something other than follow a link, described the way a
     * screen reader reads it: `expanded` says which of the two states it would
     * leave the control in, and the string is what the reader hears alongside it.
     *
     * Separate from `linkPress` because a link announces itself as a link and this
     * announces itself as a button that collapses — which is a different thing to
     * hear, and the whole point of marking it up at all.
     */
    val togglePress: (() -> Unit)? = null,
    val expanded: Boolean? = null,
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
    val visibleBounds = Rect()
    val visible = host.getLocalVisibleRect(visibleBounds) && Rect.intersects(bounds, visibleBounds)
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
      isEnabled = host.isEnabled
      if (entry.linkPress != null) {
        isClickable = true
        addAction(AccessibilityNodeInfo.ACTION_CLICK)
        AccessibilityNodeInfoCompat.wrap(this).roleDescription =
          host.context.getString(com.facebook.react.R.string.link_description)
      }
      if (entry.togglePress != null) {
        // A summary is a control, not a line of text: without this the only way to
        // reach a body with a screen reader is to guess which words are the
        // disclosure. `stateDescription` is what says which way it points, and it
        // is the state the control would leave in, which is what an announcement
        // after the press then describes.
        isClickable = true
        addAction(AccessibilityNodeInfo.ACTION_CLICK)
        AccessibilityNodeInfoCompat.wrap(this).roleDescription = "button"
        AccessibilityNodeInfoCompat.wrap(this).stateDescription =
          if (entry.expanded == true) "expanded" else "collapsed"
      }
      isVisibleToUser = visible
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
    if (resetFocus || (focused >= 0 && focused >= entryCount())) focused = -1
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
