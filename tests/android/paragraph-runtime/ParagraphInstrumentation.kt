package com.downstream

import android.app.Instrumentation
import android.os.Bundle
import android.util.Log

/** Runs production Kotlin against Android's actual text and rasterization implementation. */
class ParagraphInstrumentation : Instrumentation() {
  private var pagesOnly = false
  private var codeOnly = false
  override fun onCreate(arguments: Bundle?) {
    super.onCreate(arguments)
    pagesOnly = arguments?.getString("pagesOnly") == "true"
    codeOnly = arguments?.getString("codeOnly") == "true"
    start()
  }

  override fun onStart() {
    val result = Bundle()
    try {
      val checks = if (pagesOnly || codeOnly) org.json.JSONObject().put("paragraphChecksSkipped", true)
        else org.json.JSONObject(runParagraphChecks(this))
      if (!codeOnly) checks.put("pagedText", runPagedTextChecks(this))
      if (!pagesOnly) checks.put("codeBody", runCodeBodyChecks(this))
      if (!pagesOnly) checks.put("proseTail", runProseTailChecks(this))
      result.putString("stream", checks.toString())
      finish(0, result)
    } catch (failure: Throwable) {
      result.putString("stream", Log.getStackTraceString(failure))
      finish(1, result)
    }
  }
}
