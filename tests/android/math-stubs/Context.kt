package android.content

import android.content.res.AssetManager
import android.content.res.Resources

open class Context {
  val applicationContext get() = this
  val assets = AssetManager()
  val resources = Resources()
}
