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
    DownstreamNative.registerCodeHighlighterIfInstalled()
  }

  override fun createNativeModules(reactContext: ReactApplicationContext): List<NativeModule> {
    SessionFrameClock.install(reactContext)
    return emptyList()
  }

  override fun createViewManagers(reactContext: ReactApplicationContext): List<ViewManager<*, *>> {
    // Strict New Architecture hosts skip legacy createNativeModules.
    SessionFrameClock.install(reactContext)
    return listOf(
      DownstreamMarkdownViewManager(),
      // The slot a hosted fence's component lives in. A plain view manager: the
      // slot carries only the code block's ordinal, and the Markdown view's own
      // manager is what mounts it.
      FenceSlotViewManager(),
    )
  }
}
