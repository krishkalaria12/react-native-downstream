package android.view

class ViewTreeObserver {
  fun interface OnPreDrawListener { fun onPreDraw(): Boolean }
  var isAlive = true
  val listeners = mutableListOf<OnPreDrawListener>()
  fun addOnPreDrawListener(listener: OnPreDrawListener) { listeners.add(listener) }
  fun removeOnPreDrawListener(listener: OnPreDrawListener) { listeners.remove(listener) }
  fun dispatchPreDraw() = listeners.toList().all { it.onPreDraw() }
}
