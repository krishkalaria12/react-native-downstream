package androidx.core.view.accessibility
// Only link-role metadata uses AndroidX; plain paragraph provider is production.
class AccessibilityNodeInfoCompat {
  var roleDescription: CharSequence? = null
  var stateDescription: CharSequence? = null
  companion object { fun wrap(info: android.view.accessibility.AccessibilityNodeInfo) = AccessibilityNodeInfoCompat() }
}
