package android.view

import android.content.Context
import android.graphics.Canvas
import android.graphics.drawable.GradientDrawable

object Gravity {
  const val CENTER_VERTICAL = 16; const val START = 3; const val CENTER_HORIZONTAL = 1; const val END = 5
}
open class View(val context: Context) {
  val resources get() = context.resources
  var visibility = VISIBLE
  var recordedBackground: GradientDrawable? = null
  var contentDescription: CharSequence? = null
  var minimumWidth = 0
  var layoutParams = LayoutParams(0, 0)
  var width = 0
  var height = 0
  var measuredWidth = 0
  var measuredHeight = 0
  val paddingLeft = 0; val paddingRight = 0; val paddingTop = 0; val paddingBottom = 0
  var layoutRequests = 0
  var invalidations = 0
  open fun requestLayout() { layoutRequests++ }
  open fun invalidate() { invalidations++ }
  open fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
    setMeasuredDimension(MeasureSpec.getSize(widthMeasureSpec), MeasureSpec.getSize(heightMeasureSpec))
  }
  open fun onLayout(changed: Boolean, left: Int, top: Int, right: Int, bottom: Int) {}
  open fun onDraw(canvas: Canvas) {}
  open fun onDetachedFromWindow() {}
  open fun onAttachedToWindow() {}
  fun measure(width: Int, height: Int) { onMeasure(width, height) }
  fun layout(left: Int, top: Int, right: Int, bottom: Int) {
    width = right - left; height = bottom - top
    onLayout(true, left, top, right, bottom)
  }
  fun setMeasuredDimension(width: Int, height: Int) { measuredWidth = width; measuredHeight = height }
  open class LayoutParams(var width: Int, var height: Int) {
    companion object { const val WRAP_CONTENT = -2; const val MATCH_PARENT = -1 }
  }
  object MeasureSpec {
    const val EXACTLY = 0x40000000
    fun getSize(spec: Int) = spec and 0x3fffffff
    fun makeMeasureSpec(size: Int, mode: Int) = size or mode
  }
  companion object { const val VISIBLE = 0; const val GONE = 8; const val OVER_SCROLL_NEVER = 2 }
}
