package com.downstream

import android.content.Context
import android.os.Handler
import android.os.Looper
import android.view.Choreographer
import android.view.WindowManager
import com.facebook.react.bridge.LifecycleEventListener
import com.facebook.react.bridge.ReactApplicationContext
import com.facebook.react.common.LifecycleState

/** One-shot display ticks, with asynchronous progress while the host is paused. */
internal object SessionFrameClock {
  private val handler = Handler(Looper.getMainLooper())
  // All clock state is confined to the main thread.
  private var active = false
  private var frameQueued = false
  private var contextEpoch = 0
  private var installedContext: java.lang.ref.WeakReference<ReactApplicationContext>? = null
  // The display's rate, read at most once a second: asking the window manager on
  // every tick is a display-info query on the UI thread each frame.
  private var refreshRate = 60f
  private var refreshRateRead = false
  private var refreshRateReadNanos = 0L
  private val frame = Choreographer.FrameCallback { frameTimeNanos ->
    if (!frameQueued) return@FrameCallback
    frameQueued = false
    if (!refreshRateRead || frameTimeNanos - refreshRateReadNanos >= 1_000_000_000L) {
      val context = DownstreamNative.preparationContext?.get()
      @Suppress("DEPRECATION")
      refreshRate = (context?.getSystemService(Context.WINDOW_SERVICE) as? WindowManager)
        ?.defaultDisplay?.refreshRate?.takeIf { it.isFinite() && it > 0f } ?: 60f
      refreshRateRead = true
      refreshRateReadNanos = frameTimeNanos
    }
    val intervalMs = 1000.0 / refreshRate
    val late = (System.nanoTime() - frameTimeNanos) / 1_000_000.0 > intervalMs * 0.75
    DownstreamNative.sessionFrame(intervalMs, late)
  }

  fun install(context: ReactApplicationContext) {
    handler.post {
      if (installedContext?.get() === context) return@post
      installedContext = java.lang.ref.WeakReference(context)
      val epoch = ++contextEpoch
      if (context.lifecycleState == LifecycleState.RESUMED) active = true else pause()
      context.addLifecycleEventListener(object : LifecycleEventListener {
        override fun onHostResume() {
          if (epoch == contextEpoch) active = true
        }

        override fun onHostPause() {
          if (epoch == contextEpoch) pause()
        }

        override fun onHostDestroy() {
          if (epoch == contextEpoch) pause()
        }
      })
    }
  }

  private fun pause() {
    active = false
    if (frameQueued) {
      Choreographer.getInstance().removeFrameCallback(frame)
      frameQueued = false
      // Drain a native request already waiting for a display that may stop.
      request()
    }
  }

  fun request() {
    handler.post {
      if (!active) {
        // Explicit flush/finish must progress even without an attached view or
        // display ticks. This turn only publishes/dispatches; parsing stays on
        // the C++ worker pool. Foreground requests return to real vsync.
        DownstreamNative.sessionFrame(1000.0 / 60.0, false)
      } else if (!frameQueued) {
        frameQueued = true
        Choreographer.getInstance().postFrameCallback(frame)
      }
    }
  }
}
