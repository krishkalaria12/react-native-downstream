package android.view.animation

class PathInterpolator(private val x1: Float, private val y1: Float, private val x2: Float, private val y2: Float) {
  fun getInterpolation(progress: Float): Float {
    evaluations++
    return com.downstream.RevealMath.solve(progress, x1, y1, x2, y2)
  }
  companion object { var evaluations = 0 }
}
