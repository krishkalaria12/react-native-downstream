package com.downstream

import android.app.Instrumentation
import android.os.Bundle
import android.util.Log

/** Runs production Kotlin against Android's actual text and rasterization implementation. */
class ParagraphInstrumentation : Instrumentation() {
  private var pagesOnly = false
  override fun onCreate(arguments: Bundle?) {
    super.onCreate(arguments)
    pagesOnly = arguments?.getString("pagesOnly") == "true"
    start()
  }

  override fun onStart() {
    val result = Bundle()
    try {
      val checks = if (pagesOnly) org.json.JSONObject().put("paragraphChecksSkipped", true)
        else org.json.JSONObject(runParagraphChecks(this))
      checks.put("pagedText", runPagedTextChecks(this))
      result.putString("stream", checks.toString())
      finish(0, result)
    } catch (failure: Throwable) {
      result.putString("stream", Log.getStackTraceString(failure))
      finish(1, result)
    }
  }
}
