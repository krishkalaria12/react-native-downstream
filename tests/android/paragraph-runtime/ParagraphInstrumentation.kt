package com.downstream

import android.app.Instrumentation
import android.os.Bundle
import android.util.Log

/** Runs production Kotlin against Android's actual text and rasterization implementation. */
class ParagraphInstrumentation : Instrumentation() {
  override fun onCreate(arguments: Bundle?) {
    super.onCreate(arguments)
    start()
  }

  override fun onStart() {
    val result = Bundle()
    try {
      result.putString("stream", runParagraphChecks(this))
      finish(0, result)
    } catch (failure: Throwable) {
      result.putString("stream", Log.getStackTraceString(failure))
      finish(1, result)
    }
  }
}
