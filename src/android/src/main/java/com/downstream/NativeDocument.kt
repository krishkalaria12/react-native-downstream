package com.downstream

import android.content.Context
import android.os.Build
import com.facebook.proguard.annotations.DoNotStrip
import com.facebook.react.bridge.ReadableMap
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
  /**
   * Each block's formula, one entry per block per [MATH_STRIDE] fields.
   *
   * A separate table from [layouts] because these are the document's own ems
   * rather than the theme's resolved metrics: one table per source of truth, so a
   * theme change cannot be confused with a formula laying out.
   */
  private val maths: DoubleArray,
  /**
   * Every formula in the slice, block and inline, with its display list, as
   * `appendDisplayList` in `DownstreamJni.cpp` writes the records.
   */
  private val formulaTable: DoubleArray = DoubleArray(0),
) {
  /** One formula: where it sits, and its display list when it laid out. */
  class Formula(val block: Int, val start: Int, val end: Int, val list: MathDisplayList?)

  /** The formulas in this slice, in document order, decoded once. */
  val formulas: List<Formula> by lazy {
    val found = ArrayList<Formula>()
    var at = 0
    while (at + 4 <= formulaTable.size) {
      val block = formulaTable[at].toInt()
      val start = formulaTable[at + 1].toInt()
      val end = formulaTable[at + 2].toInt()
      val length = formulaTable[at + 3].toInt()
      val list = if (length > 0) MathDisplayList.decode(formulaTable, at + 4, length) else null
      found.add(Formula(block, start, end, list))
      at += 4 + length.coerceAtLeast(0)
    }
    found
  }

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
   * The formula's advance in ems, or zero when this block has none.
   *
   * A block with no formula reads as zero rather than throwing, so a caller that
   * reached this without checking [blockKind] gets a formula that takes no room
   * rather than an exception — which is the answer that keeps the text readable.
   */
  fun blockMathWidthEm(block: Int) = mathAt(block, 0)

  /** The height above the baseline in ems. The baseline sits at `y = height`. */
  fun blockMathHeightEm(block: Int) = mathAt(block, 1)

  /** The depth below the baseline in ems. */
  fun blockMathDepthEm(block: Int) = mathAt(block, 2)

  /**
   * Whether the formula was laid out.
   *
   * False means either that it has not been laid out yet — a stream still arriving
   * — or that it failed, and [blockMathError] is what tells the two apart.
   */
  fun blockMathResolved(block: Int) = mathAt(block, 3) != 0.0

  /** Whether the source was a display delimiter rather than an inline one. */
  fun blockMathDisplay(block: Int) = mathAt(block, 4) != 0.0

  /** The TeX between the delimiters, or empty when this block has no formula. */
  fun blockMathTex(block: Int): String {
    val index = mathAt(block, 5).toInt()
    return if (index >= 0 && index < strings.size) strings[index] else ""
  }

  /** The engine's message when the formula failed, or empty. */
  fun blockMathError(block: Int): String {
    val index = mathAt(block, 6).toInt()
    return if (index >= 0 && index < strings.size) strings[index] else ""
  }

  /**
   * The spoken form, from the engine's own verbalizer.
   *
   * Computed rather than carried, because it is a function of the TeX and the
   * display flag and would be a string per block in the document read for something
   * only a screen reader asks for. Empty when there is no formula, or when the
   * native call is unavailable, which the view falls back to the literal from.
   */
  fun blockMathSpeech(block: Int): String {
    if (blockKind(block) != KIND_MATH_BLOCK) {
      return ""
    }
    val tex = blockMathTex(block)
    if (tex.isEmpty()) {
      return ""
    }
    return DownstreamNative.mathSpeech(tex, blockMathDisplay(block))
  }

  /**
   * One field of block [block]'s formula.
   *
   * A short table falls back to zeros rather than throwing: a maths table that is
   * absent or short means the read came from a native library built without math,
   * and a block that has no formula should read as no formula either way.
   */
  private fun mathAt(block: Int, field: Int): Double {
    val offset = block * MATH_STRIDE + field
    return if (offset >= 0 && offset < maths.size) maths[offset] else 0.0
  }

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

    /**
     * One block's formula as [widthEm, heightEm, depthEm, resolved, display, tex,
     * error], seven fields each.
     *
     * The width, height and depth are the layout's own ems with the baseline at
     * `y = height`, which is what `mathBlockHeight` expects on both platforms. `tex`
     * and `error` are indexes into `strings`, or -1 when there is none, so a
     * formula that has not failed does not carry an empty string per block.
     *
     * `resolved` is separate from the ems because a formula that has not been laid
     * out and one that failed are different answers: the first is still arriving,
     * the second is a literal.
     *
     * The stride is the only place that order is written down, and it has to match
     * `DownstreamJni.cpp` exactly.
     */
    private const val MATH_STRIDE = 7

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
    /** A lone display formula, promoted to a block of its own. */
    const val KIND_MATH_BLOCK = 6
    /** A paragraph holding nothing but pictures, promoted to a block of its own. */
    const val KIND_IMAGE = 7

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
    const val STYLE_SUPERSCRIPT = 1 shl 8
    const val STYLE_SUBSCRIPT = 1 shl 9

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

/**
 * What a segment is, as `SegmentKind` in `SegmentPlan.h` numbers it.
 *
 * A kind rather than a table and a code flag, because the plan has five and two
 * more were added for math and images; a boolean per kind is how the fifth one gets
 * forgotten.
 */
internal const val SEGMENT_TEXT = 0
internal const val SEGMENT_TABLE = 1
internal const val SEGMENT_CODE = 2
internal const val SEGMENT_MATH = 3
internal const val SEGMENT_IMAGE = 4

private val SEGMENT_KINDS = intArrayOf(SEGMENT_TEXT, SEGMENT_TABLE, SEGMENT_CODE, SEGMENT_MATH, SEGMENT_IMAGE)

/** A formula block's own metrics, as the theme resolves them. */
internal data class MathMetrics(
  val padding: Float,
  /** `math.textAlign`: 0 left, 1 center, 2 right. */
  val textAlign: Int,
  val marginTop: Float,
  val marginBottom: Float,
)

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
  /** One of `SEGMENT_TEXT`, `SEGMENT_TABLE`, `SEGMENT_CODE`, `SEGMENT_MATH`, `SEGMENT_IMAGE`. */
  val kind: Int,
  /**
   * This segment's first picture's index in `Document::images`, or -1.
   *
   * Counted by the plan rather than by the view, so a segment view matches a
   * picture to its state entry without walking the document. An image segment
   * holds exactly one block, so one ordinal is the whole of it.
   */
  val imageOrdinal: Int,
  /**
   * This table's ordinal among the document's tables, which a reader's expand
   * choice is keyed by, or -1 for any other segment. Counted by the plan for the
   * same reason [codeOrdinal] is: a measurement needs it, and a measurement runs
   * before any view has walked the segments.
   */
  val tableOrdinal: Int,
)

/**
 * Every image the document holds, as the engine recorded them
 * (`Document::images`).
 *
 * What is not here is each image's state: that belongs to a view rather than to
 * the document, and it arrives in the layout map instead. So this read is the
 * same for a view that has loaded its pictures and one that has not, which is why
 * it is a document read and not a state read.
 */
internal class NativeImages(
  val urls: Array<String>,
  val alts: Array<String>,
  val titles: Array<String>,
  /**
   * [start, end, loadable, link] per image, four fields each. `loadable` is the
   * engine's own policy answer rather than the view's, and `link` is the
   * enclosing link's index, or -1 for a picture that is not inside one.
   */
  private val records: IntArray,
) {
  val count: Int get() = urls.size

  fun url(ordinal: Int) = urls[ordinal]

  /** Plain alt text, as written. Empty is a real answer, not a missing one. */
  fun alt(ordinal: Int) = alts[ordinal]

  /** The optional title, which is never drawn. */
  fun title(ordinal: Int) = titles[ordinal]

  /** Where the `![…](…)` sits in the source, for an event that reports a press. */
  fun range(ordinal: Int) = records[ordinal * 4] until records[ordinal * 4 + 1] + 1

  /**
   * Whether the engine's own policy allows this destination: HTTPS, complete,
   * with a host and no control characters or spaces.
   */
  fun loadable(ordinal: Int) = records[ordinal * 4 + 2] != 0

  /** The enclosing link's index, or -1. */
  fun link(ordinal: Int) = records[ordinal * 4 + 3]

  companion object {
    /**
     * A document with no pictures.
     *
     * An answer rather than a null, because a document with no images is ordinary
     * and every caller here has to do something sensible with one.
     */
    val EMPTY = NativeImages(emptyArray(), emptyArray(), emptyArray(), IntArray(0))
  }
}

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
    nativeInstallClipboard()
  }

  /**
   * The context worker threads build renderings with: the latest one Fabric
   * measured with, so prepared renderings use the same cache key.
   */
  @Volatile
  var preparationContext: WeakReference<Context>? = null

  /**
   * Puts [text] on the system clipboard, for the link-safety confirmation's
   * "Copy link".
   *
   * Called from C++ (`Clipboard.h`'s writer) on whatever thread JavaScript asked
   * from, so it attaches rather than assuming it is on a Java thread. The
   * clipboard manager is process-wide and holds no reference to the context it
   * was given, so the context here is the preparation one and a reader who has
   * never rendered a Markdown view copies nothing rather than crashing.
   */
  @JvmStatic
  @DoNotStrip
  fun writeClipboard(text: String): Boolean {
    val context = preparationContext?.get() ?: return false
    return try {
      val manager =
        context.getSystemService(Context.CLIPBOARD_SERVICE) as? android.content.ClipboardManager ?: return false
      manager.setPrimaryClip(android.content.ClipData.newPlainText(null, text))
      true
    } catch (error: RuntimeException) {
      false
    }
  }

  /**
   * Builds and measures a document's rendering on a worker thread after it is
   * parsed (DownstreamTextLayoutManager.cpp), so Fabric's measurement on the
   * JavaScript thread and the view's mount find it in the cache.
   *
   * [layout] is the same map `measure` and `updateState` carry: the width, the
   * code layout, the caret, and the image answers. One shape rather than a
   * positional list, so a worker cannot prepare a layout that differs from the
   * one the view draws by an argument the two callers happened to pass in a
   * different order.
   */
  @JvmStatic
  @DoNotStrip
  fun prepare(documentId: Long, layout: ReadableMap) {
    val context = preparationContext?.get() ?: return
    try {
      // The caret is read here rather than left to a later measure, because its
      // placeholder is part of the text this pass lays out: prepared without it,
      // the height a worker records is one the view never draws.
      val caret = PlacedCaret(
        layout.numberAt("caretKind", 0.0).toInt(),
        layout.numberAt("caretPlace", 0.0).toInt(),
      )
      val rendered = RenderedDocument.obtain(
        context,
        documentId,
        layout.numberAt("fontScale", 1.0).toFloat(),
        themeId = layout.numberAt("themeId", 0.0).toLong(),
        caret = caret,
      ) ?: return
      val width = layout.numberAt("width", 0.0).toFloat()
      if (width.isFinite() && width > 0) {
        rendered.height(
          PixelUtil.toPixelFromDIP(width.toDouble()).toInt(),
          layout.numberAt("chrome", 0.0).toInt(),
          layout.toCodeLayout(),
          layout.toImageLayout(),
        )
      }
    } catch (error: RuntimeException) {
      // Measurement builds the rendering again on its own thread.
    }
  }

  /**
   * The content rectangle of every hosted React child, as `[ordinal, left, top,
   * right, bottom]` groups in points.
   *
   * Called from `measure` (through C++) because `FabricUIManager.measure`
   * returns a packed size and nothing else: only `measureText` reports
   * attachment positions, and a hosted child is not text. The rendering is the
   * one measurement just filled, cached by document and width, so this reads
   * the frames it stacked rather than laying anything out again.
   *
   * [layout] is the same map `measure` and `updateState` carry, and [width] is
   * the width Yoga measured at, in points as the shadow node holds it. It is
   * converted to pixels here exactly as `FabricUIManager.measure` converts it
   * before `measure`, so this reads the rendering that measurement filled rather
   * than laying the document out again at a width the view never draws.
   */
  @JvmStatic
  @DoNotStrip
  fun hostFrames(documentId: Long, layout: ReadableMap, width: Float): FloatArray {
    val context = preparationContext?.get() ?: return FloatArray(0)
    return try {
      val rendered = RenderedDocument.obtain(
        context,
        documentId,
        layout.numberAt("fontScale", 1.0).toFloat(),
        themeId = layout.numberAt("themeId", 0.0).toLong(),
      ) ?: return FloatArray(0)
      val hosts = layout.toHostLayout()
      if (hosts.isEmpty()) {
        return FloatArray(0)
      }
      val frames = rendered.hostFrames(
        PixelUtil.toPixelFromDIP(width).toInt(),
        layout.numberAt("chrome", 0.0).toInt(),
        layout.toCodeLayout(),
        layout.toImageLayout(),
        hosts,
      )
      // Every fifth value is an ordinal, not a length: converting it with the
      // edges would hand a chart another fence's rectangle.
      FloatArray(frames.size) { index ->
        if (index % 5 == 0) frames[index].toFloat() else PixelUtil.toDIPFromPixel(frames[index].toFloat())
      }
    } catch (error: RuntimeException) {
      FloatArray(0)
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
      maths = parts[8] as DoubleArray,
      formulaTable = parts.getOrNull(9) as? DoubleArray ?: DoubleArray(0),
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
        SEGMENT_KINDS.getOrElse(values[it + 6].toInt()) { SEGMENT_TEXT },
        values[it + 7].toInt(),
        values[it + 8].toInt(),
      )
    }
  }

  /**
   * The theme's image metrics, in points, as `theme_layout::imageMetrics` resolves
   * them. Resolved in C++ so a measurement and a draw read one value.
   */
  fun imageMetrics(id: Long): ImageMetrics {
    val values = nativeImageMetrics(id) ?: return ImageMetrics()
    return ImageMetrics(
      maxHeight = values.getOrElse(0) { 0.0 }.toFloat(),
      borderRadius = values.getOrElse(1) { 8.0 }.toFloat(),
      marginTop = values.getOrElse(2) { 0.0 }.toFloat(),
      marginBottom = values.getOrElse(3) { 0.0 }.toFloat(),
      fallbackTextHeight = values.getOrElse(4) { kImageFallbackLineHeight.toDouble() }.toFloat(),
    )
  }

  /**
   * The theme's math metrics: vertical padding, text alignment as 0, 1, or 2, and
   * the margins above and below.
   *
   * Read in C++ so a measurement and a draw agree, and so Android and iOS read one
   * value rather than the same slot in two places.
   */
  fun mathMetrics(id: Long): MathMetrics {
    val values = nativeMathMetrics(id)
    return MathMetrics(
      padding = values?.getOrElse(0) { 0.0 }?.toFloat() ?: 0f,
      textAlign = values?.getOrElse(1) { 1.0 }?.toInt() ?: 1,
      marginTop = values?.getOrElse(2) { 0.0 }?.toFloat() ?: 0f,
      marginBottom = values?.getOrElse(3) { 0.0 }?.toFloat() ?: 0f,
    )
  }

  /** Returns null when the document is no longer alive. */
  fun readImages(id: Long): NativeImages? {
    val parts = nativeReadImages(id) ?: return null
    @Suppress("UNCHECKED_CAST")
    return NativeImages(
      urls = parts[0] as Array<String>,
      alts = parts[1] as Array<String>,
      titles = parts[2] as Array<String>,
      records = parts[3] as IntArray,
    )
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

  // ----------------------------------------------------------------- the fade

  /**
   * A tracker for one view, as a handle into the JNI layer's table.
   *
   * A tracker is per view rather than per document or per session, because what it
   * holds is the document *this* view last showed. Two views of the same
   * controlled source therefore each fade their own copy, which is what a
   * controlled view has always done on both platforms.
   */
  fun revealCreate(): Long = nativeRevealCreate()

  /** Releases one view's tracker, so a recycled view hands the slot on. */
  fun revealDispose(handle: Long) = nativeRevealDispose(handle)

  /**
   * Starts the tracker from what a session has already put on screen, so a message
   * scrolled back into view does not fade all of itself again. A null or empty
   * [sessionId] seeds nothing, which is the controlled case.
   */
  fun revealSeed(handle: Long, sessionId: String?) =
    nativeRevealSeed(handle, sessionId ?: "")

  /**
   * Aligns a newly applied document against the one this view last showed, and
   * answers `[ranges, starts]`: `[block, start, end]` per unit and each unit's
   * start time, in the order the engine scheduled them. Null means nothing is
   * fading.
   */
  fun revealApply(
    handle: Long,
    documentId: Long,
    now: Double,
    timing: DoubleArray,
  ): Array<Any>? {
    val result = nativeRevealApply(handle, documentId, now, timing) ?: return null
    return arrayOf(
      result.getOrNull(0) as? LongArray ?: LongArray(0),
      result.getOrNull(1) as? DoubleArray ?: DoubleArray(0),
    )
  }

  /**
   * Records a document as shown without scheduling anything.
   *
   * This is the not-animating case, and it is what stops a fade from being turned
   * on later over a document that has been on screen all along.
   */
  fun revealNoteShown(handle: Long, documentId: Long, sessionId: String?) =
    nativeRevealNoteShown(handle, documentId, sessionId ?: "")

  /**
   * Drops the units that have finished, and answers whether any are left, which is
   * what the tick asks each frame.
   */
  fun revealPrune(handle: Long, now: Double, timing: DoubleArray): Boolean =
    nativeRevealPrune(handle, now, timing)

  /** Forgets what was shown, for a new generation, a rebind or a recycled view. */
  fun revealClear(handle: Long) = nativeRevealClear(handle)

  /**
   * The platform's animator duration scale.
   *
   * Read on the UI thread once per apply rather than observed: it changes only
   * from Settings, and a fade already running is finished when it does. Zero means
   * the reader has turned animations off, which the plan turns into no fade at
   * all rather than a fade with no duration.
   */
  fun animatorDurationScale(context: android.content.Context): Float {
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O && !android.animation.ValueAnimator.areAnimatorsEnabled()) {
      return 0f
    }
    return try {
      android.provider.Settings.Global.getFloat(
        context.contentResolver,
        android.provider.Settings.Global.ANIMATOR_DURATION_SCALE,
        1f,
      )
    } catch (error: android.provider.Settings.SettingNotFoundException) {
      1f
    }
  }

  private const val PLAN_STRIDE = 9

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

  /** The image download name, using the shared URL, caption and MIME rules. */
  @JvmStatic
  fun imageName(url: String, alt: String, mimeType: String, configured: String): String? =
    nativeImageName(url, alt, mimeType, configured)

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
  private external fun nativeImageName(url: String, alt: String, mimeType: String, configured: String): String?

  /** The spoken form of a formula, from `math/MathSpeech.cpp`. */
  fun mathSpeech(tex: String, display: Boolean): String =
    nativeMathSpeech(tex, display) ?: ""

  /** The math package's marker class, which autolinking puts on the classpath. */
  private const val MATH_PACKAGE_CLASS = "com.downstream.math.DownstreamMathPackage"

  /**
   * Registers RaTeX with the engine when `react-native-downstream-math` is
   * installed.
   *
   * Registering loads nothing: the engine opens `libratex_ffi.so` the first
   * time a document needs a formula laid out, so an app that installs the package
   * and never shows math never maps the library. The package's presence is its
   * class on the classpath, which is what autolinking adds along with the library.
   */
  fun registerMathEngineIfInstalled() {
    val installed = try {
      Class.forName(MATH_PACKAGE_CLASS, false, DownstreamNative::class.java.classLoader)
      true
    } catch (error: ClassNotFoundException) {
      false
    }
    if (installed) {
      nativeRegisterMathEngine("libratex_ffi.so", "fonts")
    }
  }

  @JvmStatic
  private external fun nativeRegisterMathEngine(library: String, fontDirectory: String)

  /**
   * Records a finished image load in the engine's size cache, which the shadow node
   * measures from. A zero size records a failure.
   */
  fun recordImage(url: String, width: Int, height: Int) = nativeRecordImage(url, width, height)

  @JvmStatic
  private external fun nativeRecordImage(url: String, width: Int, height: Int)

  @JvmStatic
  private external fun nativeMathSpeech(tex: String, display: Boolean): String?

  @JvmStatic
  private external fun nativeImageMetrics(id: Long): DoubleArray?

  @JvmStatic
  private external fun nativeMathMetrics(id: Long): DoubleArray?

  @JvmStatic
  private external fun nativeReadImages(id: Long): Array<Any>?

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
  private external fun nativeRevealCreate(): Long

  @JvmStatic
  private external fun nativeRevealDispose(handle: Long)

  @JvmStatic
  private external fun nativeRevealSeed(handle: Long, sessionId: String)

  @JvmStatic
  private external fun nativeRevealApply(
    handle: Long, documentId: Long, now: Double, timing: DoubleArray,
  ): Array<Any>?

  @JvmStatic
  private external fun nativeRevealNoteShown(handle: Long, documentId: Long, sessionId: String)

  @JvmStatic
  private external fun nativeRevealPrune(handle: Long, now: Double, timing: DoubleArray): Boolean

  @JvmStatic
  private external fun nativeRevealClear(handle: Long)

  @JvmStatic
  private external fun nativeInstallPreparer()

  @JvmStatic
  private external fun nativeInstallClipboard()
}
