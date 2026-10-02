package android.graphics.drawable

class GradientDrawable {
  init { constructions++ }
  var cornerRadius = 0f
  var fillColor = 0
  fun setColor(color: Int) { fillColor = color }
  companion object { var constructions = 0 }
}
