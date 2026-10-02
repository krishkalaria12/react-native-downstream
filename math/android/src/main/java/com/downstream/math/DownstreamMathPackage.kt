package com.downstream.math

import com.facebook.react.ReactPackage
import com.facebook.react.bridge.NativeModule
import com.facebook.react.bridge.ReactApplicationContext
import com.facebook.react.uimanager.ViewManager

/**
 * The math package's presence, and nothing else.
 *
 * Autolinking needs a package class to include this library in the app's build,
 * which is what brings RaTeX's library and fonts into the APK. Downstream looks
 * for this class by name: finding it means the engine is there to load, and the
 * library itself is loaded with `dlopen` the first time a document needs a
 * formula laid out, so a process that never shows math never maps it.
 */
class DownstreamMathPackage : ReactPackage {
  override fun createNativeModules(reactContext: ReactApplicationContext): List<NativeModule> = emptyList()

  override fun createViewManagers(reactContext: ReactApplicationContext): List<ViewManager<*, *>> = emptyList()
}
