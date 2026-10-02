package android.text.style

import android.graphics.Canvas
import android.graphics.Paint

abstract class ReplacementSpan {
  abstract fun getSize(paint: Paint, text: CharSequence?, start: Int, end: Int, fm: Paint.FontMetricsInt?): Int
  abstract fun draw(canvas: Canvas, text: CharSequence?, start: Int, end: Int, x: Float, top: Int, y: Int, bottom: Int, paint: Paint)
}
