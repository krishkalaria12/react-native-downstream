package android.widget

import android.content.Context
import android.graphics.Paint
import android.view.View

open class FrameLayout(context: Context) : View(context) {
  val children = mutableListOf<View>()
  fun addView(view: View) { children.add(view) }
  fun addView(view: View, params: View.LayoutParams) { view.layoutParams = params; addView(view) }
  class LayoutParams(width: Int, height: Int) : View.LayoutParams(width, height) {
    companion object { const val WRAP_CONTENT = -2; const val MATCH_PARENT = -1 }
  }
}
class HorizontalScrollView(context: Context) : FrameLayout(context) {
  var isHorizontalScrollBarEnabled = false
  var isFillViewport = false
  var overScrollMode = 0
  var scrollResets = 0
  fun scrollTo(x: Int, y: Int) { scrollResets++ }
}
class TextView(context: Context) : View(context) {
  var includeFontPadding = true
  var gravity = 0
  val paint = Paint()
  var text: CharSequence = ""
  @JvmField var textColor = 0
  fun setTextSize(unit: Int, size: Float) { paint.textSize = size; requestLayout(); invalidate() }
  fun setTextColor(value: Int) { textColor = value; invalidate() }
}
