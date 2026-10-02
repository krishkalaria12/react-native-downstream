package com.downstream

import com.facebook.react.ReactPackage
import com.facebook.react.bridge.NativeModule
import com.facebook.react.bridge.ReactApplicationContext
import com.facebook.react.uimanager.ViewManager

class DownstreamPackage : ReactPackage {
  init {
    // Autolinking builds the package list before any JavaScript runs, so the
    // engine is registered before the first `mathAvailable()` asks for it.
    DownstreamNative.registerMathEngineIfInstalled()
  }

  override fun createNativeModules(reactContext: ReactApplicationContext): List<NativeModule> = emptyList()

  override fun createViewManagers(reactContext: ReactApplicationContext): List<ViewManager<*, *>> =
    listOf(DownstreamMarkdownViewManager())
}
