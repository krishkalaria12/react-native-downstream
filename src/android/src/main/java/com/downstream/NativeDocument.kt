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
  private val tokens: IntArray,
  /**
   * Each block's resolved typography and margins, one entry per block per
   * [BLOCK_LAYOUT_STRIDE] fields. Resolved in C++ by the same rules iOS calls, so
   * a theme means one thing rather than one thing per platform.
   * A final field holds the preceding block's bottom margin for this slice.
   */
  private val layouts: DoubleArray,
) {
  val blockCount: Int get() = blocks.size / BLOCK_STRIDE
  val runCount: Int get() = runs.size / RUN_STRIDE
  val tokenCount: Int get() = tokens.size / TOKEN_STRIDE

  /** The preceding block's resolved bottom margin, in points before scaling. */
  val previousBottomMargin: Float
    get() = layouts.getOrElse(blockCount * BLOCK_LAYOUT_STRIDE) { 0.0 }.toFloat()

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

  /**
   * Which block an offset in the flattened text falls in.
   *
   * A run's face starts as its block's, so a run has to find the block it sits
   * in. Find the last start at or before the offset without building an index.
   * Offsets before the first block and an empty document return zero.
   */
  fun blockOf(offset: Int): Int {
    var low = 0
    var high = blockCount
    while (low < high) {
      val middle = low + (high - low) / 2
      if (blockStart(middle) <= offset) {
        low = middle + 1
      } else {
        high = middle
      }
    }
    return (low - 1).coerceAtLeast(0)
  }

  /** The fence language as written, or empty for an indented block. */
  fun blockLanguage(block: Int) = blocks[block * BLOCK_STRIDE + 12].let { if (it >= 0) strings[it] else "" }

  /** A block's font size, in points before font scaling. */
  fun blockFontSize(block: Int) = layout(block, 0).toFloat()

  /** A block's weight, as a CSS weight. */
  fun blockFontWeight(block: Int) = layout(block, 1).toFloat()

  /** An index into the theme's family list, or -1 for the platform's font. */
  fun blockFontFamily(block: Int) = layout(block, 2).toInt()

  /** A block's line height, or zero to leave the platform's own leading. */
  fun blockLineHeight(block: Int) = layout(block, 3).toFloat()

  /** The color slot a block's text draws in. */
  fun blockColorSlot(block: Int) = layout(block, 4).toInt()

  /** The margin slot above a block. */
  fun blockMarginTop(block: Int) = layout(block, 5).toInt()

  /** The margin slot below a block. */
  fun blockMarginBottom(block: Int) = layout(block, 6).toInt()

  /** Whether a block draws in italic. */
  fun blockItalic(block: Int) = layout(block, 7) != 0.0

  /** Which element a block draws as. See `theme_layout::BlockElement`. */
  fun blockElement(block: Int) = layout(block, 8).toInt()

  /** Whether a block draws as code, which is what picks the monospace face. */
  fun isCodeElement(block: Int) = blockElement(block) == ELEMENT_CODE

  private fun layout(block: Int, field: Int): Double =
    layouts.getOrElse(block * BLOCK_LAYOUT_STRIDE + field) { FALLBACK_LAYOUT[field] }

  /**
   * The fence meta the engine parsed from this block's info string, so nothing
   * here scans that string a second way.
   */
  fun blockFenceMeta(block: Int) = FenceMeta(
    title = blocks[block * BLOCK_STRIDE + 13].let { if (it >= 0) strings[it] else "" },
    startLine = blocks[block * BLOCK_STRIDE + 14],
    noLineNumbers = blocks[block * BLOCK_STRIDE + 15] != 0,
  )

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

  fun tokenStart(token: Int) = tokens[token * TOKEN_STRIDE]
  fun tokenEnd(token: Int) = tokens[token * TOKEN_STRIDE + 1]
  fun tokenClass(token: Int) = tokens[token * TOKEN_STRIDE + 2]

  companion object {
    private const val BLOCK_STRIDE = 16
    private const val CELL_STRIDE = 3
    private const val CONTAINER_STRIDE = 5
    private const val RUN_STRIDE = 4
    private const val TOKEN_STRIDE = 3
    /**
     * One block's resolved layout, in the order `writeBlockLayout` writes it.
     * The stride is the only place that order is written down, and it has to
     * match `theme_layout::kBlockLayoutStride` exactly.
     */
    private const val BLOCK_LAYOUT_STRIDE = 9

    /** `theme_layout::BlockElement`. A block draws as code at this index. */
    private const val ELEMENT_CODE = 2

    /**
     * What a block reads as when its layout is missing.
     *
     * An array shorter than the blocks it describes means the read came back
     * from a build without the layouts, so the block gets the body metrics
     * rather than a zero size and a zero margin.
     */
    private val FALLBACK_LAYOUT = doubleArrayOf(16.0, 400.0, -1.0, 0.0, 9.0, 12.0, 0.0, 0.0, 0.0)

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

    // Token classes, in the order src/cpp/downstream/Document.h declares them.
    const val TOKEN_PLAIN = 0
    const val TOKEN_COMMENT = 1
    const val TOKEN_KEYWORD = 2
    const val TOKEN_TYPE = 3
    const val TOKEN_STRING = 4
    const val TOKEN_NUMBER = 5
    const val TOKEN_CONSTANT = 6
    const val TOKEN_REGEX = 7
    const val TOKEN_FUNCTION = 8
    const val TOKEN_PROPERTY = 9
    const val TOKEN_OPERATOR = 10
    const val TOKEN_PREPROCESSOR = 11
    const val TOKEN_TAG = 12
    const val TOKEN_ATTRIBUTE = 13
    const val TOKEN_INSERTED = 14
    const val TOKEN_DELETED = 15
    const val TOKEN_HEADING = 16
    const val TOKEN_COUNT = 17
  }
}

/** One segment of a document's layout plan (SegmentPlan.h). */
internal class PlannedSegment(
  val from: Int,
  val to: Int,
  val isTable: Boolean,
  val isCommitted: Boolean,
  val isCode: Boolean,
  val identity: Long,
  /** Leading blocks of the document no later input can change. */
  val settledBlocks: Int,
  /**
   * The code block's ordinal among the document's code blocks, which a reader's
   * wrap choice is keyed by, or -1 for any other segment. The plan counts them
   * as it is built, so a wrap choice resolves without walking the document.
   */
  val codeOrdinal: Int,
)

internal class NativeLink(val url: String, val start: Int, val end: Int)

/** Formats `nativeExport` produces, matching `ExportFormat` in the engine. */
internal const val EXPORT_TEXT = 0
internal const val EXPORT_CSV = 1
internal const val EXPORT_TSV = 2
internal const val EXPORT_MARKDOWN = 3
internal const val EXPORT_HTML = 4

/** JNI calls this on the caller for small exports, or on a worker for large ones. */
@DoNotStrip
internal fun interface NativeControlExportCallback {
  @DoNotStrip
  fun onResult(text: String?, html: String?, error: String?)
}

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
   * JavaScript thread and the view's mount find it in the cache.
   *
   * [width] is in points, as Fabric passes it to measure. The whole code layout
   * comes along, because a prepared height is only the height of the layout the
   * reader actually has.
   */
  @JvmStatic
  @DoNotStrip
  fun prepare(
    documentId: Long,
    fontScale: Float,
    width: Float,
    chrome: Int,
    codeBlockWrap: Boolean,
    lineNumbers: Boolean,
    codeBlockMaxHeight: Float,
    wrapToggles: IntArray,
    expanded: IntArray,
    themeId: Long,
  ) {
    val context = preparationContext?.get() ?: return
    try {
      val rendered = RenderedDocument.obtain(context, documentId, fontScale, themeId = themeId) ?: return
      if (width.isFinite() && width > 0) {
        val layout = CodeLayout(
          CodeLayoutProps(codeBlockWrap, lineNumbers, PixelUtil.toPixelFromDIP(codeBlockMaxHeight.toDouble()).toFloat()),
          CodeChoices(wrapToggles, expanded),
        )
        rendered.height(PixelUtil.toPixelFromDIP(width).toInt(), chrome, layout)
      }
    } catch (error: RuntimeException) {
      // Measurement builds the rendering again on its own thread.
    }
  }

  /** Returns null when the document is no longer alive. */
  fun readDocument(id: Long, from: Int, to: Int, themeId: Long): NativeDocument? {
    val parts = nativeReadDocument(id, from, to, themeId) ?: return null
    @Suppress("UNCHECKED_CAST")
    return NativeDocument(
      text = parts[0] as String,
      blocks = parts[1] as IntArray,
      containers = parts[2] as IntArray,
      runs = parts[3] as IntArray,
      strings = parts[4] as Array<String>,
      cells = parts[5] as IntArray,
      tokens = parts[6] as IntArray,
      layouts = parts[7] as DoubleArray,
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
        values[it + 4].toInt(),
        values[it + 5].toInt(),
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

  /**
   * Interns a theme and returns its id, which is what the state carries.
   *
   * A shadow node calls this once per prop change; the drawing layer looks the
   * result up again rather than receiving 232 numbers on every state update.
   * Empty arrays are the default theme, so an app that passes neither prop pays
   * nothing here either.
   */
  fun internTheme(slotIds: IntArray, values: DoubleArray, families: Array<String>): Long =
    nativeInternTheme(slotIds, values, families)

  /**
   * The theme's resolved values, as [NativeTheme] reads them.
   *
   * See the JNI side for the shape: five parallel arrays, one crossing, so a
   * rendering costs one trip rather than one per slot.
   */
  fun themeSnapshot(themeId: Long): Array<Any> =
    nativeThemeSnapshot(themeId) ?: Array(0) { 0.0 }

  /**
   * The platform font scale, capped by the theme's `maxFontSizeMultiplier`.
   *
   * The one theme value that changes measurement, which is why it crosses here
   * rather than being duplicated in Kotlin. An unknown id leaves the platform's
   * own scale alone.
   */
  fun fontScaleCap(themeId: Long, platformScale: Float): Float =
    nativeFontScaleCap(themeId, platformScale.toDouble()).toFloat()

  private const val PLAN_STRIDE = 6

  // Implemented in DownstreamJni.cpp, which links into libappmodules.so.
  @JvmStatic
  private external fun nativeReadDocument(id: Long, from: Int, to: Int, themeId: Long): Array<Any>?

  @JvmStatic
  private external fun nativeSegmentPlan(id: Long): LongArray?

  /**
   * Serializes blocks [from, to) of a document. `format` is an `ExportFormat`
   * and `separator` a UTF-16 unit, so both platforms produce identical bytes.
   * Returns null when the document is gone or the range is not one.
   */
  @JvmStatic
  fun export(id: Long, from: Int, to: Int, format: Int, separator: Char): String? =
    nativeExport(id, from, to, format, separator.code)

  /** Captures the snapshot before returning; newer streaming states cannot expire it. */
  fun exportControl(
    id: Long,
    from: Int,
    to: Int,
    kind: Int,
    format: Int,
    separator: Char,
    includeHtml: Boolean,
    callback: NativeControlExportCallback,
  ) = nativeExportControl(id, from, to, kind, format, separator.code, includeHtml, callback)

  /** The file name a code download saves as, extension included. */
  @JvmStatic
  fun downloadName(id: Long, block: Int, configured: String, fallback: String): String? =
    nativeDownloadName(id, block, configured, fallback)

  /** The file name a table download saves as, extension included. */
  @JvmStatic
  fun tableName(configured: String, extension: String): String? = nativeTableName(configured, extension)

  @JvmStatic
  private external fun nativeExport(id: Long, from: Int, to: Int, format: Int, separator: Int): String?

  @JvmStatic
  private external fun nativeExportControl(
    id: Long, from: Int, to: Int, kind: Int, format: Int, separator: Int,
    includeHtml: Boolean, callback: NativeControlExportCallback,
  )

  @JvmStatic
  private external fun nativeDownloadName(id: Long, block: Int, configured: String, fallback: String): String?

  @JvmStatic
  private external fun nativeTableName(configured: String, extension: String): String?

  @JvmStatic
  private external fun nativeReadLink(id: Long, index: Int): Array<Any>?

  @JvmStatic
  private external fun nativeTraceDocument(id: Long, event: String)

  @JvmStatic
  private external fun nativeIsTracingDocument(id: Long): Boolean

  // Implemented in DownstreamTextLayoutManager.cpp.
  @JvmStatic
  private external fun nativeInternTheme(slotIds: IntArray, values: DoubleArray, families: Array<String>): Long

  @JvmStatic
  private external fun nativeThemeSnapshot(themeId: Long): Array<Any>?

  @JvmStatic
  private external fun nativeFontScaleCap(themeId: Long, platformScale: Double): Double

  @JvmStatic
  private external fun nativeInstallPreparer()
}
