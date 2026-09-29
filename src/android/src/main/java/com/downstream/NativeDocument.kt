package com.downstream

import android.content.Context
import com.facebook.proguard.annotations.DoNotStrip
import com.facebook.react.uimanager.PixelUtil
import java.lang.ref.WeakReference

/**
 * A range of a parsed document's blocks, flattened by the native engine
 * (DownstreamJni.cpp). Block indices and text offsets are relative to the
 * range. Table layouts must match that file.
 */
internal class NativeDocument(
  val text: String,
  private val blocks: IntArray,
  private val containers: IntArray,
  private val runs: IntArray,
  private val strings: Array<String>,
  private val cells: IntArray,
) {
  val blockCount: Int get() = blocks.size / BLOCK_STRIDE
  val runCount: Int get() = runs.size / RUN_STRIDE

  fun blockKind(block: Int) = blocks[block * BLOCK_STRIDE]
  fun headingLevel(block: Int) = blocks[block * BLOCK_STRIDE + 1]
  fun isTableHeader(block: Int) = blocks[block * BLOCK_STRIDE + 2] != 0
  fun spacingBefore(block: Int) = blocks[block * BLOCK_STRIDE + 3]
  fun blockStart(block: Int) = blocks[block * BLOCK_STRIDE + 4]
  fun blockEnd(block: Int) = blocks[block * BLOCK_STRIDE + 5]
  fun containerCount(block: Int) = blocks[block * BLOCK_STRIDE + 7]
  fun cellCount(block: Int) = blocks[block * BLOCK_STRIDE + 9]
  fun tableId(block: Int) = blocks[block * BLOCK_STRIDE + 10]

  /** The synthetic title paragraph of an admonition, decided document-wide. */
  fun isAdmonitionTitle(block: Int) = blocks[block * BLOCK_STRIDE + 11] != 0

  private fun cell(block: Int, column: Int) = (blocks[block * BLOCK_STRIDE + 8] + column) * CELL_STRIDE

  fun cellStart(block: Int, column: Int) = cells[cell(block, column)]
  fun cellEnd(block: Int, column: Int) = cells[cell(block, column) + 1]
  fun cellAlign(block: Int, column: Int) = cells[cell(block, column) + 2]

  private fun container(block: Int, depth: Int) = (blocks[block * BLOCK_STRIDE + 6] + depth) * CONTAINER_STRIDE

  fun containerKind(block: Int, depth: Int) = containers[container(block, depth)]
  fun containerId(block: Int, depth: Int) = containers[container(block, depth) + 1]
  fun showsMarker(block: Int, depth: Int) = containers[container(block, depth) + 2] != 0
  fun taskState(block: Int, depth: Int) = containers[container(block, depth) + 3]
  fun marker(block: Int, depth: Int): String? = containers[container(block, depth) + 4].let { if (it >= 0) strings[it] else null }

  fun runStart(run: Int) = runs[run * RUN_STRIDE]
  fun runEnd(run: Int) = runs[run * RUN_STRIDE + 1]
  fun runStyles(run: Int) = runs[run * RUN_STRIDE + 2]
  fun runLink(run: Int) = runs[run * RUN_STRIDE + 3]

  companion object {
    private const val BLOCK_STRIDE = 12
    private const val CELL_STRIDE = 3
    private const val CONTAINER_STRIDE = 5
    private const val RUN_STRIDE = 4

    const val KIND_PARAGRAPH = 0
    const val KIND_HEADING = 1
    const val KIND_CODE = 2
    const val KIND_HTML = 3
    const val KIND_RULE = 4
    const val KIND_TABLE_ROW = 5

    const val CONTAINER_QUOTE = 0
    const val CONTAINER_LIST_ITEM = 1
    const val CONTAINER_ADMONITION = 2
    const val CONTAINER_FOOTNOTE = 3

    const val ALIGN_CENTER = 2
    const val ALIGN_RIGHT = 3

    const val TASK_UNCHECKED = 1
    const val TASK_CHECKED = 2

    const val SPACING_NONE = 0
    const val SPACING_TIGHT = 1

    const val STYLE_STRONG = 1
    const val STYLE_EMPHASIS = 1 shl 1
    const val STYLE_STRIKETHROUGH = 1 shl 2
    const val STYLE_CODE = 1 shl 3
    const val STYLE_IMAGE_ALT = 1 shl 5
    const val STYLE_FOOTNOTE = 1 shl 6
  }
}

/** One segment of a document's layout plan (SegmentPlan.h). */
internal class PlannedSegment(
  val from: Int,
  val to: Int,
  val isTable: Boolean,
  val isCommitted: Boolean,
  val followsCode: Boolean,
  val identity: Long,
)

internal class NativeLink(val url: String, val start: Int, val end: Int)

internal object DownstreamNative {
  init {
    nativeInstallPreparer()
  }

  /**
   * The context worker threads build renderings with: the latest one Fabric
   * measured with, so prepared renderings use the same cache key.
   */
  @Volatile
  var preparationContext: WeakReference<Context>? = null

  /**
   * Builds and measures a document's rendering on a worker thread after it is
   * parsed (DownstreamTextLayoutManager.cpp), so Fabric's measurement on the
   * JavaScript thread and the view's mount find it in the cache. [width] is
   * in points, as Fabric passes it to measure.
   */
  @JvmStatic
  @DoNotStrip
  fun prepare(documentId: Long, fontScale: Float, width: Float) {
    val context = preparationContext?.get() ?: return
    try {
      val rendered = RenderedDocument.obtain(context, documentId, fontScale) ?: return
      if (width.isFinite() && width > 0) {
        rendered.height(PixelUtil.toPixelFromDIP(width).toInt())
      }
    } catch (error: RuntimeException) {
      // Measurement builds the rendering again on its own thread.
    }
  }

  /** Returns null when the document is no longer alive. */
  fun readDocument(id: Long, from: Int, to: Int): NativeDocument? {
    val parts = nativeReadDocument(id, from, to) ?: return null
    @Suppress("UNCHECKED_CAST")
    return NativeDocument(
      text = parts[0] as String,
      blocks = parts[1] as IntArray,
      containers = parts[2] as IntArray,
      runs = parts[3] as IntArray,
      strings = parts[4] as Array<String>,
      cells = parts[5] as IntArray,
    )
  }

  /** Returns null when the document is no longer alive. */
  fun readSegmentPlan(id: Long): List<PlannedSegment>? {
    val values = nativeSegmentPlan(id) ?: return null
    return (values.indices step PLAN_STRIDE).map {
      val flags = values[it + 2].toInt()
      PlannedSegment(
        values[it].toInt(),
        values[it + 1].toInt(),
        flags and 1 != 0,
        flags and 2 != 0,
        flags and 4 != 0,
        values[it + 3],
      )
    }
  }

  /** Returns null when the document or link is gone. */
  fun readLink(id: Long, index: Int): NativeLink? {
    val parts = nativeReadLink(id, index) ?: return null
    val range = parts[1] as IntArray
    return NativeLink(parts[0] as String, range[0], range[1])
  }

  fun traceDocument(id: Long, event: String) = nativeTraceDocument(id, event)
  fun isTracingDocument(id: Long) = nativeIsTracingDocument(id)

  private const val PLAN_STRIDE = 4

  // Implemented in DownstreamJni.cpp, which links into libappmodules.so.
  @JvmStatic
  private external fun nativeReadDocument(id: Long, from: Int, to: Int): Array<Any>?

  @JvmStatic
  private external fun nativeSegmentPlan(id: Long): LongArray?

  @JvmStatic
  private external fun nativeReadLink(id: Long, index: Int): Array<Any>?

  @JvmStatic
  private external fun nativeTraceDocument(id: Long, event: String)

  @JvmStatic
  private external fun nativeIsTracingDocument(id: Long): Boolean

  // Implemented in DownstreamTextLayoutManager.cpp.
  @JvmStatic
  private external fun nativeInstallPreparer()
}
