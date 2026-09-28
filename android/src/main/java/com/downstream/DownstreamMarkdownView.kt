package com.downstream

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Canvas
import android.os.Bundle
import android.text.StaticLayout
import android.view.MotionEvent
import android.view.View
import android.view.accessibility.AccessibilityNodeInfo

/**
 * Draws a rendered document with the same StaticLayout configuration that
 * measured it. The view never parses; documents arrive through Fabric state.
 */
class DownstreamMarkdownView(context: Context) : View(context) {
  internal var onLinkPress: ((view: DownstreamMarkdownView, url: String, start: Int, end: Int) -> Unit)? = null

  private var documentId = 0L
  private var fontScale = 1f
  private var rendered: RenderedDocument? = null
  private var layout: StaticLayout? = null
  private var pressedLink = -1

  init {
    importantForAccessibility = IMPORTANT_FOR_ACCESSIBILITY_YES
  }

  fun setDocument(documentId: Long, fontScale: Float) {
    if (documentId == this.documentId && fontScale == this.fontScale && rendered != null) {
      return
    }
    this.documentId = documentId
    this.fontScale = fontScale
    rendered = if (documentId != 0L) RenderedDocument.obtain(context, documentId, fontScale) else null
    layout = null
    contentDescription = rendered?.accessibilityBlocks?.joinToString("\n")
    invalidate()
  }

  fun reset() {
    documentId = 0L
    rendered = null
    layout = null
    pressedLink = -1
    contentDescription = null
    invalidate()
  }

  private fun currentLayout(): StaticLayout? {
    val document = rendered ?: return null
    val width = width - paddingLeft - paddingRight
    val existing = layout
    if (existing != null && existing.width == width) {
      return existing
    }
    return document.createLayout(width).also { layout = it }
  }

  override fun onDraw(canvas: Canvas) {
    super.onDraw(canvas)
    val layout = currentLayout() ?: return
    canvas.save()
    canvas.translate(paddingLeft.toFloat(), paddingTop.toFloat())
    layout.draw(canvas)
    canvas.restore()
  }

  private fun linkAt(x: Float, y: Float): Int {
    val layout = currentLayout() ?: return -1
    val localX = x - paddingLeft
    val localY = y - paddingTop
    if (localY < 0 || localY > layout.height) {
      return -1
    }
    val line = layout.getLineForVertical(localY.toInt())
    if (localX < layout.getLineLeft(line) - HIT_SLOP || localX > layout.getLineRight(line) + HIT_SLOP) {
      return -1
    }
    val offset = layout.getOffsetForHorizontal(line, localX)
    val spans = layout.text.let { it as? android.text.Spanned }?.getSpans(offset, offset, LinkSpan::class.java)
    return spans?.firstOrNull()?.index ?: -1
  }

  @SuppressLint("ClickableViewAccessibility")
  override fun onTouchEvent(event: MotionEvent): Boolean {
    when (event.actionMasked) {
      MotionEvent.ACTION_DOWN -> {
        pressedLink = linkAt(event.x, event.y)
        return pressedLink >= 0
      }
      MotionEvent.ACTION_UP -> {
        val link = pressedLink
        pressedLink = -1
        if (link >= 0 && linkAt(event.x, event.y) == link) {
          pressLink(link)
          return true
        }
      }
      MotionEvent.ACTION_CANCEL -> pressedLink = -1
    }
    return pressedLink >= 0
  }

  private fun pressLink(link: Int) {
    val document = rendered?.document ?: return
    if (link < document.linkCount) {
      onLinkPress?.invoke(this, document.linkUrl(link), document.linkRangeStart(link), document.linkRangeEnd(link))
    }
  }

  override fun onInitializeAccessibilityNodeInfo(info: AccessibilityNodeInfo) {
    super.onInitializeAccessibilityNodeInfo(info)
    val document = rendered?.document ?: return
    for (link in 0 until document.linkCount) {
      info.addAction(AccessibilityNodeInfo.AccessibilityAction(LINK_ACTION_BASE + link, "Link: ${document.linkUrl(link)}"))
    }
  }

  override fun performAccessibilityAction(action: Int, arguments: Bundle?): Boolean {
    val document = rendered?.document
    if (document != null && action >= LINK_ACTION_BASE && action < LINK_ACTION_BASE + document.linkCount) {
      pressLink(action - LINK_ACTION_BASE)
      return true
    }
    return super.performAccessibilityAction(action, arguments)
  }

  private companion object {
    const val HIT_SLOP = 8f
    const val LINK_ACTION_BASE = 0x44530000
  }
}
