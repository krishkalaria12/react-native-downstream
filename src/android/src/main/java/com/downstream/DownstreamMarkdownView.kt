package com.downstream

import android.annotation.SuppressLint
import android.content.Context
import android.content.res.ColorStateList
import android.content.res.Configuration
import android.graphics.Typeface
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.Path
import android.graphics.Rect
import android.graphics.RectF
import android.graphics.drawable.GradientDrawable
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.text.Layout
import android.text.PrecomputedText
import android.text.Spanned
import android.text.TextPaint
import android.text.TextUtils
import android.text.style.AbsoluteSizeSpan
import android.text.style.ForegroundColorSpan
import android.text.style.StrikethroughSpan
import android.util.TypedValue
import android.view.Gravity
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.widget.FrameLayout
import android.widget.HorizontalScrollView
import android.widget.ImageButton
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.PopupMenu
import android.widget.ScrollView
import android.widget.TextView
import android.view.accessibility.AccessibilityNodeInfo
import android.view.accessibility.AccessibilityNodeProvider
import java.lang.ref.WeakReference
import com.facebook.react.bridge.Arguments
import com.facebook.react.uimanager.PixelUtil
import com.facebook.react.uimanager.StateWrapper

private const val HIT_SLOP = 8f
private val controlHandler = Handler(Looper.getMainLooper())

private class ControlExportRequest(
  val sourceEpoch: Int,
  val generation: Int,
  val sessionId: String?,
  val copyEpoch: Long,
  val clipboardTimestamp: Long?,
  val isCode: Boolean,
  val download: Boolean,
  val action: String,
  val format: String,
  val language: String,
  val filename: String,
  val mime: String,
)

// Block kinds, matching `BlockKind` in the engine, so a control names the kind
// it acts on rather than a platform-local enum.
internal const val BLOCK_CODE = 2
internal const val BLOCK_TABLE_ROW = 5

// Buttons to build, as bits of `downstream::ControlBits`.
internal const val CONTROL_CODE_COPY = 1
internal const val CONTROL_CODE_DOWNLOAD = 2
internal const val CONTROL_TABLE_COPY = 4
internal const val CONTROL_TABLE_DOWNLOAD = 8
internal const val CONTROL_CODE_WRAP = 16
internal const val CONTROL_CODE_EXPAND = 32
internal const val CONTROL_IMAGE_DOWNLOAD = 64

// How long a copy button shows its check before going back, as in Streamdown.
private const val COPIED_DURATION_MS = 2_000L

// Indexes into the label array `src/controls.ts` sends, in the same order.
private val DEFAULT_LABELS = listOf(
  "Copy Code", "Download file", "Copied", "Wrap lines", "Don't wrap lines",
  // Appended, not inserted: `src/controls.ts` sends the labels positionally, so a
  // new key goes at the end rather than moving every later one.
  "Show all lines", "Show fewer lines",
  "Copy table", "Copy table as Markdown", "Copy table as CSV", "Copy table as TSV",
  "Download table", "Download table as CSV", "Download table as Markdown",
  "Markdown", "CSV", "TSV",
  "Image not available", "Download image", "Image", "Image blocked",
)
private const val LABEL_COPY_CODE = 0
private const val LABEL_DOWNLOAD_FILE = 1
private const val LABEL_COPIED = 2
private const val LABEL_WRAP_CODE = 3
private const val LABEL_UNWRAP_CODE = 4
private const val LABEL_EXPAND_CODE = 5
private const val LABEL_COLLAPSE_CODE = 6
private const val LABEL_COPY_TABLE = 7
private const val LABEL_DOWNLOAD_TABLE = 11
private const val LABEL_FORMAT_MARKDOWN = 14
private const val LABEL_FORMAT_CSV = 15
private const val LABEL_FORMAT_TSV = 16
private const val LABEL_IMAGE_NOT_AVAILABLE = 17
private const val LABEL_IMAGE_DOWNLOAD = 18
private const val LABEL_IMAGE_BLOCKED = 20

/** How far a line number is dimmed below the code it numbers. Mirrors `kGutterAlpha` on iOS. */
private const val GUTTER_ALPHA = 0.5f

/**
 * What a control button did. The segment views hold no document, so they hand
 * the host a description and the host reads the block.
 */
internal interface ControlHost {
  fun onControl(
    kind: Int,
    blockIndex: Int,
    blockCount: Int,
    language: String,
    action: String,
    format: String,
  )

  fun onToggleWrap(ordinal: Int, language: String, wrapped: Boolean)
  fun onToggleExpand(ordinal: Int, language: String, expanded: Boolean)
  fun onControlFailed(kind: Int, action: String, message: String)
}

// A control is square and fills the 32dp chrome row it sits in, with its glyph
// drawn at 18dp, about the size of the iOS symbol.
private const val CONTROL_BUTTON_DP = 32f
private const val CONTROL_GLYPH_DP = 18f

/** One code header or table toolbar button, sized in dp and tinted later. */
internal fun newControlButton(context: Context, drawable: Int): ImageButton = ImageButton(context).apply {
  val density = context.resources.displayMetrics.density
  val size = (CONTROL_BUTTON_DP * density).toInt()
  val inset = ((CONTROL_BUTTON_DP - CONTROL_GLYPH_DP) / 2 * density).toInt()
  layoutParams = LinearLayout.LayoutParams(size, size)
  setImageResource(drawable)
  scaleType = ImageView.ScaleType.FIT_CENTER
  setPadding(inset, inset, inset, inset)
  val ripple = TypedValue()
  context.theme.resolveAttribute(android.R.attr.selectableItemBackgroundBorderless, ripple, true)
  setBackgroundResource(ripple.resourceId)
}

/** Link index at a point in a laid-out text, or -1. */
private fun linkAt(layout: Layout, x: Float, y: Float): Int {
  if (y < 0 || y > layout.height) {
    return -1
  }
  val line = layout.getLineForVertical(y.toInt())
  if (x < layout.getLineLeft(line) - HIT_SLOP || x > layout.getLineRight(line) + HIT_SLOP) {
    return -1
  }
  val offset = layout.getOffsetForHorizontal(line, x)
  val text = layout.text as? Spanned ?: return -1
  return text.getSpans(offset, offset, LinkSpan::class.java).firstOrNull()?.index ?: -1
}

/**
 * Fabric host view for <DownstreamMarkdown>: stacks segment views at the
 * frames the measurement computed. Documents arrive through Fabric state; the
 * view never parses.
 */
class DownstreamMarkdownView(context: Context) : ViewGroup(context), ControlHost {
  internal var onLinkPress: ((view: DownstreamMarkdownView, url: String, start: Int, end: Int) -> Unit)? = null
  internal var onControl:
    ((view: DownstreamMarkdownView, block: String, action: String, format: String, language: String,
      filename: String, url: String) -> Unit)? = null
  internal var onControlFailure:
    ((view: DownstreamMarkdownView, block: String, action: String, message: String) -> Unit)? = null
  internal var onWrapToggle: ((view: DownstreamMarkdownView, ordinal: Int, wrapped: Boolean) -> Unit)? = null
  internal var onExpandToggle: ((view: DownstreamMarkdownView, ordinal: Int, expanded: Boolean) -> Unit)? = null

  private var documentId = 0L
  private var fontScale = 1f
  /** The interned theme the state carried, which the rendering was built with. */
  private var themeId = 0L
  /**
   * The theme props, kept as sent and interned only when a state has not yet
   * named a theme. Props arrive before the state that records what the node
   * resolved them to, so this is the fallback for that window and nothing else:
   * once a state has named a theme, its id is the authority, because that is the
   * theme the node measured with.
   */
  internal var themeSlots: IntArray? = null
  internal var themeValues: DoubleArray? = null
  internal var themeFonts: Array<String>? = null
  private var rendered: RenderedDocument? = null
  private var selectable = true
  private var syntaxHighlighting = true
  // Every control on, which is the default and matches the codegen default.
  private var controls = CONTROL_CODE_COPY or CONTROL_CODE_DOWNLOAD or CONTROL_TABLE_COPY or
    CONTROL_TABLE_DOWNLOAD or CONTROL_CODE_WRAP or CONTROL_CODE_EXPAND or CONTROL_IMAGE_DOWNLOAD
  private var controlLabels: List<String> = DEFAULT_LABELS
  private var chrome = 0
  /**
   * The whole code layout: the props and the reader's own per-block choices. One
   * value, so the frame a card is given and the layout it draws inside it are one
   * decision, and so a measurement and a mount cannot disagree about a height.
   */
  private var codeLayout = CodeLayout()
  private var csvSeparator = ","
  private var codeFilename = "file"
  private var tableFilename = "table"
  private var imageFilename = "image"
  private var mathFlags = 0
  private var allowedImagePrefixes: List<String> = emptyList()
  private var exporter = DocumentExporter(context)
  private var generation = 0
  private var sessionId: String? = null
  private var sourceEpoch = 0
  private var pendingTraceDocumentId = 0L

  fun setGeneration(value: Int) {
    if (value != generation) {
      generation = value
      sourceChanged()
    }
  }

  fun setSessionId(value: String?) {
    if (value != sessionId) {
      sessionId = value
      sourceChanged()
    }
  }

  private fun sourceChanged() {
    sourceEpoch++
    // A new generation has nothing left to pin and no offsets worth keeping, so
    // every reader scroll position goes, tables' and code cards' alike.
    for (index in 0 until childCount) {
      when (val child = getChildAt(index)) {
        is TableSegmentView -> child.resetScroll()
        is CodeSegmentView -> child.resetScroll()
      }
    }
  }

  /**
   * Mounts the document the state carries, at the font scale and with the theme
   * it carries.
   *
   * The theme id is part of the early-out, so a theme that changes but resolves
   * to the same font scale still rebuilds once and then stops: the id is what
   * says the theme is a different one, not what the rendering is.
   */
  fun setDocument(documentId: Long, fontScale: Float, stateThemeId: Long = 0L) {
    // A state that names no theme falls back to the props, interned here. The
    // default theme's id is zero, so this is also what an app that passes
    // neither prop gets, and it costs one empty intern.
    val resolved = if (stateThemeId != 0L) stateThemeId else internProps()
    if (documentId == this.documentId && fontScale == this.fontScale && resolved == this.themeId &&
      rendered != null
    ) {
      return
    }
    if (pendingTraceDocumentId != 0L && pendingTraceDocumentId != documentId) {
      DownstreamNative.traceDocument(pendingTraceDocumentId, "frame_unavailable")
      pendingTraceDocumentId = 0L
    }
    this.documentId = documentId
    this.fontScale = fontScale
    this.themeId = resolved
    val tracing = documentId != 0L && DownstreamNative.isTracingDocument(documentId)
    if (tracing) DownstreamNative.traceDocument(documentId, "apply_start")
    rendered = if (documentId != 0L) {
      RenderedDocument.obtain(context, documentId, fontScale, syntaxHighlighting, resolved)
    } else {
      null
    }
    syncSegmentViews()
    if (tracing) {
      DownstreamNative.traceDocument(documentId, "apply_end")
      pendingTraceDocumentId = documentId
      scheduleTraceFrame()
    }
  }

  /** The interned theme id for the props as sent, or zero for the default. */
  private fun internProps(): Long {
    val slots = themeSlots ?: return 0L
    val values = themeValues ?: return 0L
    return DownstreamNative.internTheme(slots, values, themeFonts ?: emptyArray())
  }

  private fun scheduleTraceFrame() {
    val pending = pendingTraceDocumentId
    if (pending == 0L || !isAttachedToWindow) return
    postOnAnimation {
      if (pendingTraceDocumentId != pending || !isAttachedToWindow) return@postOnAnimation
      pendingTraceDocumentId = 0L
      // This frame callback estimates visibility; it is not a GPU paint timestamp.
      DownstreamNative.traceDocument(pending, "frame_estimate")
    }
  }

  override fun onAttachedToWindow() {
    super.onAttachedToWindow()
    scheduleTraceFrame()
  }

  fun setSelectable(selectable: Boolean) {
    this.selectable = selectable
    applySettledState()
  }

  fun setControls(controls: Int) {
    if (controls == this.controls) return
    this.controls = controls
    applySettledState()
  }

  fun setControlLabels(value: List<String>) {
    controlLabels = if (value.size >= DEFAULT_LABELS.size) value else DEFAULT_LABELS
    for (index in 0 until childCount) {
      when (val child = getChildAt(index)) {
        is CodeSegmentView -> child.setControlLabels(controlLabels)
        is TableSegmentView -> child.setControlLabels(controlLabels)
        is ImageSegmentView -> child.setStatusLabels(
          controlLabels[LABEL_IMAGE_NOT_AVAILABLE], controlLabels[LABEL_IMAGE_BLOCKED],
        )
      }
    }
    applySettledState()
  }

  fun setCodeFilename(value: String) {
    codeFilename = value
  }

  fun setTableFilename(value: String) {
    tableFilename = value
  }

  /**
   * The name a picture's save uses when the app named none.
   *
   * The engine's `imageDownloadName` turns this into a file name from the URL's
   * extension, the destination's MIME subtype, or this as a last resort, so a
   * picture called `?id=7` still saves as something rather than as nothing.
   */
  fun setImageFilename(value: String) {
    imageFilename = value
  }

  /**
   * The math flags, as the engine's bitmask.
   *
   * Held so an unresolved formula can be told apart from one that was never meant to
   * be laid out: with the flag off the engine leaves the literal alone and this view
   * has nothing to do, and with it on an unresolved formula draws in the theme's
   * error color. Both answers are otherwise in the state.
   */
  fun setMathFlags(value: Int) {
    mathFlags = value
  }

  /** Whether pictures are fetched at all. The state says so per picture as well. */
  fun setImages(value: Boolean) {
    imagesEnabled = value
    imageLayout = if (value) imageLayout else imageLayout.copy(state = imageLayout.state.copy(enabled = false))
    syncSegmentViews()
  }

  /**
   * The prefixes a measurement resolved against.
   *
   * Kept so the value that produced the state is readable next to it: a view that
   * shows a blocked picture's alt text should be able to say which prefix list said
   * so. The list itself is not re-applied here.
   */
  fun setAllowedImagePrefixes(value: List<String>) {
    allowedImagePrefixes = value
  }

  /** The image answers with this app's policy applied, which is what frames use. */
  private var imagesEnabled = true

  fun setCsvSeparator(value: String) {
    csvSeparator = value
  }

  /**
   * The code layout props, which the shadow node measures with and the view draws
   * with. The heights here are pixels, because the platforms measure in pixels.
   */
  fun setCodeProps(wrap: Boolean, lineNumbers: Boolean, maxHeightDp: Double) {
    val props = CodeLayoutProps(
      wrap,
      lineNumbers,
      PixelUtil.toPixelFromDIP(maxHeightDp).toFloat(),
    )
    if (props == codeLayout.props) return
    codeLayout = CodeLayout(props, codeLayout.choices)
    applySettledState()
    requestLayout()
  }

  /** The reader's own per-block choices, as the state carries them. */
  fun setCodeChoices(wrapToggles: IntArray, expanded: IntArray) {
    val choices = CodeChoices(wrapToggles, expanded)
    if (choices == codeLayout.choices) return
    codeLayout = CodeLayout(codeLayout.props, choices)
    applySettledState()
    requestLayout()
  }

  fun setChrome(value: Int) {
    if (value == chrome) return
    chrome = value
    applySettledState()
  }



  /**
   * Turns on a segment's controls and its selection once every block it covers
   * is settled. A streaming tail is rebuilt on every snapshot and its selection
   * cleared, so a selection there would vanish mid-drag; earlier text is settled
   * and keeps its selection.
   */
  private fun applySettledState() {
    val plan = rendered?.plan ?: return
    for (index in 0 until minOf(childCount, plan.size)) {
      val planned = plan[index]
      val settled = planned.from + 1 <= planned.settledBlocks
      when (val child = getChildAt(index)) {
        is TextSegmentView -> child.setSelectable(selectable && settled)
        is CodeSegmentView -> {
          // The same two calls the measurement used, so the frame this card is
          // given and the layout it draws inside it are one decision.
          val ordinal = child.ordinal
          child.setControls(controls)
          child.setControlsEnabled(settled)
          if (ordinal >= 0) {
            child.setLayout(resolveCodeBlock(codeLayout, ordinal, child.fenceMeta))
          }
          // A block that is settled keeps its offset and stops following its
          // newest line. The plan carries the same settled count the parser does,
          // so a card and the parser cannot disagree about whether it closed.
          child.setStreaming(!settled)
          child.setHeaderVisible(chrome and RenderedDocument.CHROME_CODE_HEADER != 0)
        }
        is TableSegmentView -> {
          child.setControls(controls)
          child.setControlsEnabled(settled)
          child.setToolbarVisible(chrome and RenderedDocument.CHROME_TABLE_TOOLBAR != 0)
        }
        is ImageSegmentView -> child.setDownloadControl(controls, settled, controlLabels[LABEL_IMAGE_DOWNLOAD])
      }
    }
  }

  // ------------------------------------------------------------- controls

  override fun onControl(
    kind: Int,
    blockIndex: Int,
    blockCount: Int,
    language: String,
    action: String,
    format: String,
  ) {
    if (documentId == 0L) return
    val to = blockIndex + blockCount
    val isCode = kind == BLOCK_CODE
    val download = action == "download"
    val separator = if (download) ControlDelivery.resolveSeparator(csvSeparator) else ','
    // Code always copies the text it shows; a table is copied in the format its
    // menu chose.
    val exportFormat = if (isCode) EXPORT_TEXT else when (format) {
      "csv" -> EXPORT_CSV
      "tsv" -> EXPORT_TSV
      "markdown" -> EXPORT_MARKDOWN
      else -> EXPORT_TEXT
    }

    val name = if (!download) {
      ""
    } else if (isCode) {
      DownstreamNative.downloadName(documentId, blockIndex, codeFilename, "file") ?: "$codeFilename.txt"
    } else {
      DownstreamNative.tableName(tableFilename, if (format == "markdown") "md" else "csv")
        ?: "$tableFilename.${if (format == "markdown") "md" else "csv"}"
    }
    val request = ControlExportRequest(
      sourceEpoch, generation, sessionId, if (download) 0L else ControlDelivery.beginCopy(),
      if (download) null else ControlDelivery.clipboardTimestamp(context),
      isCode, download, action, format, language, name,
      if (isCode) exporter.mimeTypeFor(name, "text/plain") else "text/csv",
    )
    val weakView = WeakReference(this)
    // Native captures a strong immutable snapshot before queuing. Its callback
    // holds only a weak view, so streaming can replace state without invalidating
    // the pressed block or keeping a recycled view alive.
    DownstreamNative.exportControl(
      documentId, blockIndex, to, kind, exportFormat, separator, !download && !isCode,
    ) { text, html, failure ->
      var error = failure
      var bytes: ByteArray? = null
      if (download && text != null && error == null) {
        try {
          // Large exports invoke this on the native worker, including UTF-8
          // encoding; the UI thread only presents the system save dialog.
          bytes = if (format == "csv") {
            byteArrayOf(0xEF.toByte(), 0xBB.toByte(), 0xBF.toByte()) + text.toByteArray(Charsets.UTF_8)
          } else {
            text.toByteArray(Charsets.UTF_8)
          }
        } catch (exception: RuntimeException) {
          error = "The block could not be encoded for download."
        }
      }
      val exportedBytes = bytes
      val exportError = error
      val finish = Runnable { weakView.get()?.finishControlExport(request, text, html, exportedBytes, exportError) }
      if (Looper.myLooper() == Looper.getMainLooper()) finish.run() else controlHandler.post(finish)
    }
  }

  private fun acceptsControlExport(request: ControlExportRequest): Boolean =
    request.sourceEpoch == sourceEpoch && request.generation == generation && request.sessionId == sessionId &&
      documentId != 0L && (request.download ||
        (ControlDelivery.isCurrentCopy(request.copyEpoch) &&
          ControlDelivery.clipboardTimestamp(context) == request.clipboardTimestamp))

  private fun finishControlExport(
    request: ControlExportRequest,
    text: String?,
    html: String?,
    bytes: ByteArray?,
    error: String?,
  ) {
    if (!acceptsControlExport(request)) return
    val block = if (request.isCode) "code" else "table"
    if (error != null || text == null) {
      onControlFailure?.invoke(this, block, request.action, error ?: "The block could not be exported.")
      return
    }
    if (!request.download) {
      val copied = if (request.isCode) {
        ControlDelivery.copyText(context, request.language.ifEmpty { "Code" }, text)
      } else {
        ControlDelivery.copyTable(context, text, html.orEmpty())
      }
      if (!copied) {
        onControlFailure?.invoke(
          this, block, request.action,
          "The clipboard could not take that much text. Saving it to a file has no such limit.",
        )
        return
      }
      onControl?.invoke(this, block, request.action, request.format, request.language, "", "")
      return
    }
    val weakView = WeakReference(this)
    exporter.save(request.filename, request.mime, bytes ?: ByteArray(0)) { url, failure ->
      val view = weakView.get() ?: return@save
      if (!view.acceptsControlExport(request)) return@save
      if (failure != null) {
        view.onControlFailure?.invoke(view, block, "download", failure)
      } else if (url != null) {
        view.onControl?.invoke(view, block, request.action, request.format, request.language, request.filename, "")
      }
    }
  }

  override fun onControlFailed(kind: Int, action: String, message: String) {
    onControlFailure?.invoke(this, if (kind == BLOCK_CODE) "code" else "table", action, message)
  }

  /**
   * Rebuilds the rendering when the prop changes, because the two renderings
   * differ only in the code block colors and are cached apart.
   */
  fun setSyntaxHighlighting(enabled: Boolean) {
    if (enabled != syntaxHighlighting) {
      syntaxHighlighting = enabled
      if (documentId != 0L) {
        rendered = RenderedDocument.obtain(context, documentId, fontScale, syntaxHighlighting, themeId)
        syncSegmentViews()
      }
    }
  }

  fun reset() {
    if (pendingTraceDocumentId != 0L) {
      DownstreamNative.traceDocument(pendingTraceDocumentId, "frame_unavailable")
      pendingTraceDocumentId = 0L
    }
    documentId = 0L
    // A recycled view must not keep the theme of the message it last drew, or
    // the next one would mount with a state the reader never asked for. The
    // props go too: the next message's props may not have arrived yet, and a
    // stale theme would be drawn until they did.
    themeId = 0L
    themeSlots = null
    themeValues = null
    themeFonts = null
    rendered = null
    syntaxHighlighting = true
    generation = 0
    sessionId = null
    sourceEpoch++
    codeLayout = CodeLayout()
    onControl = null
    onControlFailure = null
    onWrapToggle = null
    onExpandToggle = null
    // A recycled view has no emitter any more. Already opened saves finish,
    // while their callbacks drop events belonging to the previous source.
    exporter.release()
    exporter = DocumentExporter(context)
    removeAllViews()
  }

  /** Invalidates pending exports and picker results when the view goes away. */
  fun release() {
    sourceEpoch++
    exporter.release()
  }

  /**
   * The Fabric state this view's layout hangs off. Held so a wrap toggle can
   * hand a new state back: Yoga, and the chat list around the message, have to
   * learn the new height, and a state update is the one path that relayouts
   * without a React render.
   */
  private var stateWrapper: StateWrapper? = null

  fun setStateWrapper(wrapper: StateWrapper?) {
    stateWrapper = wrapper
  }

  /** The generation a control failure belongs to, so a stale one is dropped. */
  fun currentGeneration(): Int = generation

  /**
   * The ordinal of the code block whose button was pressed, or -1 when the view
   * cannot tell which one it was. -1 happens after a recycle, where a toggle
   * would name a block in a document that is no longer mounted.
   *
   * One slot rather than one per button, because a press is one press and the
   * delegate reads it immediately after the callback returns.
   */
  fun nextToggledOrdinal(): Int {
    val toggled = nextOrdinal
    if (toggled < 0) return -1
    nextOrdinal = -1
    return toggled
  }

  private var nextOrdinal = -1

  override fun onToggleWrap(ordinal: Int, language: String, wrapped: Boolean) {
    nextOrdinal = ordinal
    onWrapToggle?.invoke(this, ordinal, wrapped)
  }

  override fun onToggleExpand(ordinal: Int, language: String, expanded: Boolean) {
    nextOrdinal = ordinal
    onExpandToggle?.invoke(this, ordinal, expanded)
  }

  /**
   * Hands a new state back with [ordinal]'s wrap choice toggled, through the same
   * Fabric state path the wrap button uses.
   *
   * The height changes, and only Yoga and the chat list around the message can
   * know that, so a state update is the one path that relayouts without a React
   * render. The whole code layout travels with it, because together the props and
   * the sets decide each block's height.
   */
  fun toggleWrap(ordinal: Int) = updateChoice { it.copyWith(ordinal) }

  /** The same, for the cap the expand button lifts. */
  fun toggleExpand(ordinal: Int) = updateChoice { it.copyWithExpand(ordinal) }

  private fun updateChoice(toggle: (CodeChoices) -> CodeChoices) {
    if (stateWrapper == null) return
    val choices = toggle(codeLayout.choices)
    val layout = CodeLayout(codeLayout.props, choices)
    chrome = chromeFor(layout)
    codeLayout = layout
    applySettledState()
    requestLayout()
    pushState(layout)
  }

  /**
   * Hands the state back with [layout], which is a Fabric state update: the node is
   * cloned, measured again, and reconciles its image answers from the engine's
   * cache, all without a React render.
   */
  private fun pushState(layout: CodeLayout) {
    val wrapper = stateWrapper ?: return
    val choices = layout.choices
    val map = Arguments.createMap()
    map.putDouble("documentId", documentId.toDouble())
    map.putDouble("fontScale", fontScale.toDouble())
    map.putDouble("chrome", chrome.toDouble())
    map.putDouble("codeBlockWrap", if (layout.props.wrap) 1.0 else 0.0)
    map.putDouble("lineNumbers", if (layout.props.lineNumbers) 1.0 else 0.0)
    // The cap crosses as points, because that is the unit the prop and the
    // shadow node use; this side converts to pixels when it lays out.
    map.putDouble("codeBlockMaxHeight", PixelUtil.toDIPFromPixel(layout.props.maxHeight).toDouble())
    map.putArray("wrapToggles", intArrayOf(choices.wrapToggles))
    map.putArray("expanded", intArrayOf(choices.expanded))
    wrapper.updateState(map)
  }

  /**
   * The chrome a layout draws with, with the same rule the shadow node uses: a
   * header and a toolbar are bits, and the wrap bit says every code block wraps,
   * which stops being true as soon as the reader has made one block differ from
   * the prop. A mixed document's per-block layout comes from `resolveCodeBlock`,
   * not from a bit.
   */
  private fun chromeFor(layout: CodeLayout): Int {
    var chrome = 0
    if (controls and (CONTROL_CODE_COPY or CONTROL_CODE_DOWNLOAD or CONTROL_CODE_WRAP or
      CONTROL_CODE_EXPAND) != 0
    ) {
      chrome = chrome or RenderedDocument.CHROME_CODE_HEADER
    }
    if (controls and (CONTROL_TABLE_COPY or CONTROL_TABLE_DOWNLOAD) != 0) {
      chrome = chrome or RenderedDocument.CHROME_TABLE_TOOLBAR
    }
    if (controls and CONTROL_CODE_WRAP != 0 && layout.props.wrap && layout.choices.wrapToggles.isEmpty()) {
      chrome = chrome or RenderedDocument.CHROME_CODE_WRAP
    }
    return chrome
  }

  private fun intArrayOf(values: IntArray): com.facebook.react.bridge.WritableArray =
    Arguments.createArray().also { array -> for (value in values) array.pushInt(value) }

  override fun onConfigurationChanged(newConfig: Configuration) {
    super.onConfigurationChanged(newConfig)
    if (documentId != 0L) {
      RenderedDocument.obtain(context, documentId, fontScale, syntaxHighlighting, themeId)?.let {
        rendered = it
        syncSegmentViews()
      }
    }
  }



  internal fun pressLink(index: Int) {
    val link = DownstreamNative.readLink(documentId, index) ?: return
    onLinkPress?.invoke(this, link.url, link.start, link.end)
  }

  // Reuse views of the same kind in place, so unchanged segments keep their
  // selection and scroll position across updates.
  private fun syncSegmentViews() {
    val document = rendered
    val segments = document?.segments.orEmpty()
    segments.forEachIndexed { index, segment ->
      var view = if (index < childCount) getChildAt(index) else null
      val reusable = when (segment) {
        is TextSegment -> view is TextSegmentView
        is CodeSegment -> view is CodeSegmentView
        is TableSegment -> view is TableSegmentView
        is ImageSegment -> view is ImageSegmentView
        is MathSegment -> view is MathSegmentView
      }
      if (!reusable) {
        if (view != null) removeViewAt(index)
        view = when (segment) {
          is TextSegment -> TextSegmentView(context)
          is CodeSegment -> CodeSegmentView(context).apply { setControlHost(this@DownstreamMarkdownView) }
          is TableSegment -> TableSegmentView(context).apply { setControlHost(this@DownstreamMarkdownView) }
          is ImageSegment -> ImageSegmentView(context).also { new ->
            // A picture's answer is the engine's, so a load that finishes is reported
            // upward and not applied here: the view rebuilds from the state rather
            // than resizing itself, which is what keeps a measured height and a drawn
            // one the same number.
            new.onImageAnswer = { url, answer -> reportImageAnswer(url, answer) }
            new.onImagePress = { url, alt, title, range -> pressImage(url, alt, title, range) }
            new.onImageDownload = { image -> downloadImage(image) }
          }
          is MathSegment -> MathSegmentView(context)
        }
        addView(view, index)
      }
      when (segment) {
        is TextSegment -> (view as TextSegmentView).setSegment(segment, document!!)
        is CodeSegment -> (view as CodeSegmentView).setSegment(segment, document!!)
        is TableSegment -> (view as TableSegmentView).setSegment(segment, sourceEpoch, document!!.style.chromeRowHeight)
        // Read once into the local rather than off `rendered` per field: it is a
        // mutable property, so the compiler will not smart-cast it, and five reads
        // of a field another thread can replace is five chances to mix one
        // snapshot's metrics with another's.
        is ImageSegment -> (view as ImageSegmentView).apply {
          val style = document!!.style
          setStyle(style.imageMetrics, style.imageAltColor)
          setStatusLabels(controlLabels[LABEL_IMAGE_NOT_AVAILABLE], controlLabels[LABEL_IMAGE_BLOCKED])
          setImageLayout(imageLayout)
          setSegment(segment)
        }
        is MathSegment -> (view as MathSegmentView).apply {
          val style = document!!.style
          setStyle(
            style.mathFontSize,
            style.mathColor,
            style.mathBackground,
            style.mathErrorColor,
            style.mathPadding,
            style.mathRadius,
          )
          setSegment(segment)
        }
      }
    }
    while (childCount > segments.size) {
      removeViewAt(childCount - 1)
    }
    applySettledState()
    layoutSegments()
  }

  /**
   * The engine's answers about pictures, from the state the shadow node resolved.
   *
   * Held here rather than per segment because a state update replaces the whole
   * layout map at once, and a per-segment answer would have to be matched back to a
   * view that may already have been recycled. The view group is the one thing that
   * outlives every segment.
   */
  private var imageLayout = ImageLayout()

  /** Set by the view manager: a press on a picture that is not inside a link. */
  internal var onImagePress: ((view: DownstreamMarkdownView, url: String, alt: String, title: String, range: IntRange) -> Unit)? = null

  /**
   * Sets the answers and restacks, because a picture's height is its state.
   *
   * The frames are keyed on the answers as well as the width, so a load that
   * finishes collapses the placeholder and moves the text under it in one pass.
   */
  internal fun setImageLayout(value: ImageLayout) {
    if (imageLayout == value) {
      return
    }
    imageLayout = value
    syncSegmentViews()
  }

  /**
   * Records a picture's answer with the engine and asks the shadow node to measure
   * again.
   *
   * The size goes into the engine's process-wide cache, keyed by URL, which is what
   * the shadow node reads when it resolves the image layout; the state update is
   * only the nudge that makes it measure. So a message scrolled back into view
   * measures at its loaded height on its first frame, without loading again.
   */
  private fun reportImageAnswer(url: String, answer: AndroidImages.Answer?) {
    DownstreamNative.recordImage(url, answer?.width ?: 0, answer?.height ?: 0)
    pushState(codeLayout)
  }

  /**
   * A press on a picture that is not inside a link.
   *
   * The library opens nothing: an app that has a viewer wants its own, and one that
   * does not should not have a picture that navigates somewhere surprising.
   */
  private fun pressImage(url: String, alt: String, title: String, range: IntRange) {
    onImagePress?.invoke(this, url, alt, title, range)
  }

  private fun downloadImage(image: ImageSegment) {
    if (documentId == 0L || controls and CONTROL_IMAGE_DOWNLOAD == 0) return
    val epoch = sourceEpoch
    val requestGeneration = generation
    val requestSession = sessionId
    val configuredName = imageFilename
    val weakView = WeakReference(this)
    fun currentView(): DownstreamMarkdownView? = weakView.get()?.takeIf {
      it.documentId != 0L && it.sourceEpoch == epoch && it.generation == requestGeneration &&
        it.sessionId == requestSession
    }
    AndroidImages.download(image.url) { result ->
      val view = currentView() ?: return@download
      if (result == null) {
        view.onControlFailure?.invoke(view, "image", "download", "The image could not be fetched.")
        return@download
      }
      val name = DownstreamNative.imageName(image.url, image.alt, result.mimeType, configuredName)
        ?: "$configuredName.png"
      view.exporter.save(name, result.mimeType, result.bytes) { url, failure ->
        val owner = currentView() ?: return@save
        if (failure != null) {
          owner.onControlFailure?.invoke(owner, "image", "download", failure)
        } else if (url != null) {
          owner.onControl?.invoke(owner, "image", "download", "", "", name, image.url)
        }
      }
    }
  }

  // React Native does not relayout children that request it, so segments are
  // laid out here whenever the document or size changes.
  private fun layoutSegments() {
    val document = rendered ?: return
    val width = width - paddingLeft - paddingRight
    if (width <= 0) {
      return
    }
    val frames = document.frames(width, chrome, codeLayout, imageLayout)
    for (index in 0 until minOf(childCount, frames.size / 2)) {
      val child = getChildAt(index)
      val top = paddingTop + frames[index * 2]
      val height = frames[index * 2 + 1]
      child.measure(
        MeasureSpec.makeMeasureSpec(width, MeasureSpec.EXACTLY),
        MeasureSpec.makeMeasureSpec(height, MeasureSpec.EXACTLY),
      )
      child.layout(paddingLeft, top, paddingLeft + width, top + height)
    }
  }

  override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
    setMeasuredDimension(MeasureSpec.getSize(widthMeasureSpec), MeasureSpec.getSize(heightMeasureSpec))
  }

  override fun onLayout(changed: Boolean, left: Int, top: Int, right: Int, bottom: Int) {
    layoutSegments()
  }

  private var layoutPosted = false

  override fun requestLayout() {
    super.requestLayout()
    // Children changing text or scroll range need a pass the host never runs.
    // Runs during the superclass constructor too, before fields initialize.
    if (!layoutPosted) {
      layoutPosted = true
      post {
        layoutPosted = false
        layoutSegments()
      }
    }
  }
}

/**
 * A text segment in a TextView configured like the measuring StaticLayout
 * (RenderedDocument.buildLayout), which gives selection and text navigation.
 */
private class TextReadingPart(val piece: AccessibleTextPiece, val heading: Boolean, val taskState: Int)

@SuppressLint("ViewConstructor")
internal class TextSegmentView(context: Context) : TextView(context) {
  private var segment: TextSegment? = null
  private var pressedLink = -1
  private val codePaint = Paint()
  private val decorationPaint = TextPaint()
  private val codePath = Path()
  private val codeBounds = RectF()
  private var readingParts = emptyList<TextReadingPart>()
  private var readingBlocks = emptyList<AccessibleBlock>()
  private val accessibility = MarkdownAccessibility(this, { readingParts.size }) { accessiblePart(it) }

  init {
    includeFontPadding = false
    setPadding(0, 0, 0, 0)
    setLineSpacing(0f, 1f)
    breakStrategy = Layout.BREAK_STRATEGY_HIGH_QUALITY
    hyphenationFrequency = Layout.HYPHENATION_FREQUENCY_NONE
    textDirection = View.TEXT_DIRECTION_FIRST_STRONG_LTR
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
      isFallbackLineSpacing = true
    }
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.VANILLA_ICE_CREAM) {
      // Android 15 flips TextView#USE_BOUNDS_FOR_WIDTH on for apps targeting
      // API 35+, so a TextView breaks lines on glyph bounds where the
      // measuring StaticLayout in RenderedDocument.buildLayout breaks them on
      // glyph advances. For any font whose ink overhangs its advance, a line
      // that fits at measure time wraps at draw time, and that extra line
      // falls outside the height Yoga measured: the last word silently
      // disappears. Measurement sets setUseBoundsForWidth(false) too, so both
      // sides break lines the same way.
      //
      // The trade is Android 15's automatic reservation of overhang space at
      // the edges of a line, which is what every earlier version did not do
      // and what losing whole words is worse than.
      setUseBoundsForWidth(false)
    }
    gravity = Gravity.TOP or Gravity.START
    setBackgroundColor(0)
    highlightColor = 0x33007AFF
  }

  fun setSelectable(selectable: Boolean) {
    if (isTextSelectable != selectable) {
      setTextIsSelectable(selectable)
    }
  }

  override fun onTextContextMenuItem(id: Int): Boolean {
    val handled = super.onTextContextMenuItem(id)
    if (handled && (id == android.R.id.copy || id == android.R.id.cut)) ControlDelivery.beginCopy()
    return handled
  }

  fun setSegment(segment: TextSegment, rendered: RenderedDocument) {
    if (segment === this.segment) {
      return
    }
    setContent(segment.text, segment.accessibleBlocks, rendered)
    // setContent also serves code bodies and clears this field. Retain the
    // identity afterwards so settled text keeps its layout and selection.
    this.segment = segment
  }

  /**
   * Shows text that lives in another segment, such as a code block's body: the
   * same spans, precomputed text, and reading order, without a throwaway
   * segment object standing in for the real one.
   */
  fun setContent(text: Spanned, accessibleBlocks: List<AccessibleBlock>, rendered: RenderedDocument) {
    segment = null
    readingBlocks = accessibleBlocks
    val precomputed = Build.VERSION.SDK_INT >= Build.VERSION_CODES.P &&
      PrecomputedTexts.show(this, text, rendered.paint.color)
    if (!precomputed) {
      paint.typeface = rendered.paint.typeface
      setTextSize(TypedValue.COMPLEX_UNIT_PX, rendered.paint.textSize)
      setTextColor(rendered.paint.color)
      // Selection needs editable spans; plain display takes the cheaper
      // static layout.
      setText(text, if (isTextSelectable) BufferType.SPANNABLE else BufferType.NORMAL)
    }
    // A themed selection highlight. `textHighlightColor` is what a TextView
    // draws its selection in, and it is the only handle on it that has existed
    // across every supported API level.
    rendered.style.colors.selectionColor?.let { highlight ->
      highlightColor = highlight
    }
    readingParts = buildReadingParts(accessibleBlocks)
    accessibility.contentChanged()
  }

  override fun getAccessibilityNodeProvider(): AccessibilityNodeProvider = accessibility

  override fun onInitializeAccessibilityNodeInfo(info: AccessibilityNodeInfo) {
    super.onInitializeAccessibilityNodeInfo(info)
    // The provider exposes text and links in source order.
    info.text = null
    info.contentDescription = null
    info.isFocusable = false
  }

  private fun buildReadingParts(blocks: List<AccessibleBlock>): List<TextReadingPart> = buildList {
    val content = this@TextSegmentView.text as? Spanned ?: return@buildList
    for (block in blocks) {
      val start = block.start.coerceIn(0, content.length)
      val end = block.end.coerceIn(start, content.length)
      linkPieces(content, start, end).forEachIndexed { index, piece ->
        add(TextReadingPart(piece, index == 0 && block.heading,
          if (index == 0) block.taskState else 0))
      }
    }
  }

  private fun accessiblePart(index: Int): MarkdownAccessibility.Entry? {
    val layout = layout ?: return null
    val part = readingParts.getOrNull(index) ?: return null
    val start = part.piece.start
    val end = part.piece.end
    val content = text ?: return null
    val visible = content.subSequence(start, end).toString()
    val label = when (part.taskState) {
      NativeDocument.TASK_CHECKED -> "Checked, $visible"
      NativeDocument.TASK_UNCHECKED -> "Unchecked, $visible"
      else -> visible
    }
    val firstLine = layout.getLineForOffset(start)
    val lastLine = layout.getLineForOffset(if (end > start) end - 1 else start)
    val selectionPath = Path()
    layout.getSelectionPath(start, end, selectionPath)
    val selectedBounds = RectF()
    selectionPath.computeBounds(selectedBounds, true)
    val bounds = Rect().also { selectedBounds.roundOut(it) }
    if (bounds.isEmpty) {
      bounds.set(0, layout.getLineTop(firstLine), width, layout.getLineBottom(lastLine))
    }
    bounds.offset(0, -scrollY)
    val link = part.piece.linkIndex
    return MarkdownAccessibility.Entry(
      text = label,
      bounds = bounds,
      heading = part.heading,
      linkPress = if (link == null) null else ({ findHost()?.pressLink(link) }),
    )
  }

  override fun onDraw(canvas: Canvas) {
    drawInlineCodeBackgrounds(canvas)
    super.onDraw(canvas)
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P && text is PrecomputedText) {
      drawDecorations(canvas)
    }
  }

  // A TextView drawing PrecomputedText leaves out underline and
  // strikethrough, so link underlines and strikethrough lines are drawn here
  // at the font's own positions, under or through each line's run.
  private fun drawDecorations(canvas: Canvas) {
    val layout = layout ?: return
    val text = layout.text as? Spanned ?: return
    val links = text.getSpans(0, text.length, LinkSpan::class.java)
    val strikes = text.getSpans(0, text.length, StrikethroughSpan::class.java)
    if (links.isEmpty() && strikes.isEmpty()) {
      return
    }
    canvas.save()
    canvas.translate(totalPaddingLeft.toFloat(), totalPaddingTop.toFloat())
    for (link in links) {
      drawDecoration(canvas, layout, text, link, link.color, strike = false)
    }
    for (strike in strikes) {
      val start = text.getSpanStart(strike)
      val color = text.getSpans(start, start + 1, ForegroundColorSpan::class.java).lastOrNull()?.foregroundColor
        ?: currentTextColor
      drawDecoration(canvas, layout, text, strike, color, strike = true)
    }
    canvas.restore()
  }

  private fun drawDecoration(canvas: Canvas, layout: Layout, text: Spanned, span: Any, color: Int, strike: Boolean) {
    val start = text.getSpanStart(span)
    val end = text.getSpanEnd(span)
    if (start >= end) return
    decorationPaint.set(paint)
    text.getSpans(start, end, AbsoluteSizeSpan::class.java).lastOrNull()?.let { decorationPaint.textSize = it.size.toFloat() }
    val size = decorationPaint.textSize
    val (position, thickness) = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
      if (strike) {
        decorationPaint.strikeThruPosition to decorationPaint.strikeThruThickness
      } else {
        decorationPaint.underlinePosition to decorationPaint.underlineThickness
      }
    } else {
      (if (strike) -size * 0.28f else size / 9f) to size / 18f
    }
    decorationPaint.color = color
    decorationPaint.style = Paint.Style.FILL
    for (line in layout.getLineForOffset(start)..layout.getLineForOffset(end - 1)) {
      val from = maxOf(start, layout.getLineStart(line))
      val to = minOf(end, layout.getLineEnd(line))
      if (from >= to) continue
      codePath.reset()
      layout.getSelectionPath(from, to, codePath)
      codePath.computeBounds(codeBounds, true)
      val y = layout.getLineBaseline(line) + position
      canvas.drawRect(codeBounds.left, y, codeBounds.right, y + maxOf(1f, thickness), decorationPaint)
    }
  }

  // Each line of a code span gets a rectangle from the selection path's
  // horizontal extent, which follows wrapping and bidirectional text, and the
  // code's own ascent and descent.
  private fun drawInlineCodeBackgrounds(canvas: Canvas) {
    val layout = layout ?: return
    val text = layout.text as? Spanned ?: return
    val spans = text.getSpans(0, text.length, InlineCodeBackgroundSpan::class.java)
    if (spans.isEmpty()) {
      return
    }
    canvas.save()
    canvas.translate(totalPaddingLeft.toFloat(), totalPaddingTop.toFloat())
    for (span in spans) {
      val start = text.getSpanStart(span)
      val end = text.getSpanEnd(span)
      if (start >= end) continue
      codePaint.color = span.color
      for (line in layout.getLineForOffset(start)..layout.getLineForOffset(end - 1)) {
        val from = maxOf(start, layout.getLineStart(line))
        val to = minOf(end, layout.getLineEnd(line))
        if (from >= to) continue
        codePath.reset()
        layout.getSelectionPath(from, to, codePath)
        codePath.computeBounds(codeBounds, true)
        val baseline = layout.getLineBaseline(line).toFloat()
        canvas.drawRect(codeBounds.left, baseline + span.ascent, codeBounds.right, baseline + span.descent, codePaint)
      }
    }
    canvas.restore()
  }

  // A touch that starts on a link belongs to the link: the TextView never
  // sees it, so selection neither highlights the link nor clicks its span.
  @SuppressLint("ClickableViewAccessibility")
  override fun onTouchEvent(event: MotionEvent): Boolean {
    val layout = layout ?: return super.onTouchEvent(event)
    val x = event.x - totalPaddingLeft + scrollX
    val y = event.y - totalPaddingTop + scrollY
    if (event.actionMasked == MotionEvent.ACTION_DOWN) {
      pressedLink = linkAt(layout, x, y)
    }
    if (pressedLink < 0) {
      return super.onTouchEvent(event)
    }
    when (event.actionMasked) {
      MotionEvent.ACTION_UP -> {
        val link = pressedLink
        pressedLink = -1
        if (linkAt(layout, x, y) == link) {
          findHost()?.pressLink(link)
        }
      }
      MotionEvent.ACTION_CANCEL -> pressedLink = -1
    }
    return true
  }

  private fun findHost(): DownstreamMarkdownView? {
    var parent = parent
    while (parent != null && parent !is DownstreamMarkdownView) {
      parent = parent.parent
    }
    return parent as? DownstreamMarkdownView
  }
}


/**
 * The line numbers beside a code block, drawn rather than typed.
 *
 * A TextView here would put numbers into the selection, the clipboard, and the
 * accessibility tree, which is exactly what a line number must not be. Drawing
 * them keeps copied text byte-identical with numbers on or off, and keeps a screen
 * reader from reading them.
 *
 * The gutter scrolls vertically with the code and never sideways, so the numbers
 * stay legible while a long line scrolls under them.
 */
internal class CodeGutterView(context: Context) : View(context) {
  private var segment: CodeSegment? = null
  private var body: TextSegmentView? = null
  private var layout: CodeBlockLayout? = null
  private var bodyLayout: Layout? = null
  private var color = 0
  private val numberPaint = TextPaint(Paint.ANTI_ALIAS_FLAG)

  init {
    setWillNotDraw(false)
    // Numbers are decoration beside the code, not text in the document, so they
    // are never a focus stop and never read aloud.
    importantForAccessibility = IMPORTANT_FOR_ACCESSIBILITY_NO
    isClickable = false
    isFocusable = false
  }

  fun update(
    segment: CodeSegment,
    body: TextSegmentView,
    layout: CodeBlockLayout,
    codeTextSize: Int,
    codeFace: Typeface,
    color: Int,
  ) {
    val bodyLayout = body.layout
    if (this.segment === segment && this.body === body && this.layout == layout &&
      this.bodyLayout === bodyLayout && this.color == color &&
      numberPaint.textSize == codeTextSize.toFloat() && numberPaint.typeface == codeFace) return
    this.segment = segment
    this.body = body
    this.layout = layout
    this.bodyLayout = bodyLayout
    this.color = color
    // The code's own size and face, so a number's digits line up with the code's
    // monospaced columns and a theme that names a mono family gets its numbers in
    // it rather than in the platform default.
    numberPaint.textSize = codeTextSize.toFloat()
    numberPaint.typeface = codeFace
    numberPaint.color = color
    invalidate()
  }

  override fun onDraw(canvas: Canvas) {
    val segment = this.segment ?: return
    val layout = this.layout ?: return
    val body = this.body ?: return
    val textLayout = body.layout ?: return
    val color = this.color
    if (segment.lineStarts.isEmpty() || color == 0) return
    // The gutter is as tall as the code and sits beside it, so a line's y here is
    // its y in the body's own layout.
    val starts = segment.lineStarts
    val width = width.toFloat()

    // Locate the visible logical line in logarithmic time, then draw only the
    // numbers inside the dirty rect. The hidden prefix is never walked.
    val dirty = canvas.clipBounds
    val visibleLine = textLayout.getLineForVertical(dirty.top)
    val first = codeLineForOffset(starts, textLayout.getLineStart(visibleLine))
    for (index in first until starts.size) {
      val line = textLayout.getLineForOffset(starts[index].coerceAtMost(textLayout.text.length))
      val baseline = textLayout.getLineBaseline(line)
      if (textLayout.getLineTop(line) > dirty.bottom) break
      val number = (layout.firstNumber + index).toString()
      // Right-aligned against the gap, so the numbers line up on their last digit
      // as they do in GitHub and VS Code.
      canvas.drawText(
        number,
        width - segment.gutterGap - numberPaint.measureText(number),
        baseline.toFloat(),
        numberPaint,
      )
    }
  }
}

/**
 * The vertical scroll view around a capped code card's gutter and body.
 *
 * It exists on every card, not only a capped one, so a block that crosses the cap
 * mid-stream is never re-parented and keeps its selection and offsets.
 * Whether it scrolls at all is what differs.
 *
 * A drag belongs to this view only while it can scroll that way. The list around
 * it intercepts a vertical drag as soon as it passes the touch slop, before this
 * view's own scrolling starts, so the drag is claimed on the down and then judged
 * on each move with `requestDisallowInterceptTouchEvent`: kept while the card can
 * scroll the way the finger is going, released at the edge, where the list takes
 * it over from the next move.
 */
internal class CodeViewport(context: Context) : ScrollView(context) {
  /** A pinned card is one whose reader has not scrolled away from the bottom. */
  private var pinned = true
  /** Set when the card asks for a pin, applied at the next layout. */
  private var needsPin = false

  /** Only a touch-driven scroll unpins; a pin this view applied must not unpin itself. */
  private var touchScrolling = false

  /** The last move's y, so each move knows which way the finger is going. */
  private var lastTouchY = 0f

  /** True while this block's fence is open, which is what makes a capped card follow. */
  var streaming: Boolean = false
    set(value) {
      if (field == value) return
      field = value
      // A block that starts unsettled follows its newest line, whether or not it
      // is past its cap yet, so it follows from the moment it crosses.
      if (value) {
        pinned = true
        requestPin()
      }
    }

  init {
    isHorizontalScrollBarEnabled = false
    isFillViewport = false
    overScrollMode = OVER_SCROLL_NEVER
    // The platform's own fading edge says there is more below, which is both
    // cheaper and more familiar than a fade this view draws over its own code.
    // Enabling it is enough: the length comes from the app's own
    // `fadingEdgeLength`, so the fade matches every other scrolling view.
    isVerticalFadingEdgeEnabled = true
  }

  override fun onLayout(changed: Boolean, left: Int, top: Int, right: Int, bottom: Int) {
    super.onLayout(changed, left, top, right, bottom)
    if (!needsPin) return
    needsPin = false
    if (pinned) pinToBottom()
  }

  override fun computeScroll() {
    if (touchScrolling) {
      pinned = isNearBottom()
    }
    super.computeScroll()
  }

  override fun dispatchTouchEvent(event: MotionEvent): Boolean {
    when (event.actionMasked) {
      MotionEvent.ACTION_DOWN -> {
        lastTouchY = event.y
        // Claimed up front whenever there is somewhere to scroll, because the list
        // decides at its first move past the slop and would win the race otherwise.
        // An uncapped card never claims, which is today's behavior.
        claimDrag(canScrollVertically(1) || canScrollVertically(-1))
      }
      MotionEvent.ACTION_MOVE -> {
        val dy = event.y - lastTouchY
        lastTouchY = event.y
        // A finger moving up moves the content toward its bottom, which is the
        // positive direction for `canScrollVertically`.
        if (dy != 0f) claimDrag(canScrollVertically(if (dy < 0) 1 else -1))
      }
    }
    return super.dispatchTouchEvent(event)
  }

  private fun claimDrag(claim: Boolean) {
    parent?.requestDisallowInterceptTouchEvent(claim)
  }

  override fun onInterceptTouchEvent(event: MotionEvent): Boolean {
    if (event.actionMasked == MotionEvent.ACTION_DOWN) {
      touchScrolling = true
    }
    return super.onInterceptTouchEvent(event)
  }

  override fun onTouchEvent(event: MotionEvent): Boolean {
    if (event.actionMasked == MotionEvent.ACTION_UP || event.actionMasked == MotionEvent.ACTION_CANCEL) {
      touchScrolling = false
    }
    return super.onTouchEvent(event)
  }

  private fun isNearBottom(): Boolean {
    val range = computeVerticalScrollRange() - computeVerticalScrollExtent()
    return range - scrollY <= PIN_THRESHOLD
  }

  private fun pinToBottom() {
    val range = computeVerticalScrollRange() - computeVerticalScrollExtent()
    if (range > 0) scrollTo(0, range)
  }

  /** Moves to the newest line at the next layout, unless the reader has left. */
  fun requestPin() {
    needsPin = true
    requestLayout()
  }

  /**
   * Moves to the newest line now, unless the reader has left. For the card's own
   * layout pass, which has just laid this view's content out: a layout requested
   * from inside a React Native view is not run until the next snapshot.
   */
  fun followNewestLine() {
    needsPin = false
    if (pinned) pinToBottom()
  }

  companion object {
    /** Streamdown's threshold: how far from the bottom is still "following". */
    private const val PIN_THRESHOLD = 8
  }
}

/**
 * One code block: a header row above a card that scrolls sideways and, when the
 * block is past `codeBlockMaxHeight`, vertically inside its own frame.
 *
 * The body is a text segment view, so selection, plain-text copy, token colors,
 * and link hit testing inside the code are the ones a text segment already has.
 * The gutter is a sibling that draws numbers rather than holding text, so there is
 * nothing to select, copy, or read aloud.
 */
internal class CodeSegmentView(context: Context) : FrameLayout(context) {
  private val header = LinearLayout(context).apply {
    orientation = LinearLayout.HORIZONTAL
    gravity = Gravity.CENTER_VERTICAL
  }
  private val language = TextView(context).apply {
    setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f)
    typeface = Typeface.MONOSPACE
    gravity = Gravity.CENTER_VERTICAL or Gravity.START
    setPadding((12 * resources.displayMetrics.density).toInt(), 0, 0, 0)
    isSingleLine = true
    // A fence's title can be a long path, so it truncates in the middle and keeps
    // its file name rather than losing the name to an ellipsis.
    ellipsize = TextUtils.TruncateAt.MIDDLE
  }
  private val buttons = LinearLayout(context).apply {
    orientation = LinearLayout.HORIZONTAL
    gravity = Gravity.CENTER_VERTICAL
  }
  private val viewport = CodeViewport(context)
  private val row = LinearLayout(context).apply { orientation = LinearLayout.HORIZONTAL }
  private val gutter = CodeGutterView(context)
  private val scroll = HorizontalScrollView(context).apply {
    isHorizontalScrollBarEnabled = false
    isFillViewport = false
    overScrollMode = OVER_SCROLL_NEVER
  }
  private val body = TextSegmentView(context)
  private val host = Handler(Looper.getMainLooper())
  private val cardBackground = GradientDrawable()
  private val barPaint = Paint(Paint.ANTI_ALIAS_FLAG)

  private var segment: CodeSegment? = null
  private var rendered: RenderedDocument? = null
  private var controlLabels: List<String> = DEFAULT_LABELS
  private var controls = 0
  private var controlsEnabled = false
  private var layout: CodeBlockLayout? = null
  private var copied = false
  private var savedOffset = 0
  private var savedVerticalOffset = 0
  private var restoreScroll = false
  private var controlHost: ControlHost? = null
  /** The header row's fill, from the theme, or null where it set none. */
  private var headerBackground: Int? = null
  /** The rule under the header row, from the theme, or null where it set none. */
  private var headerBorder: Int? = null
  /** True while the fence is open, which is what makes a capped card follow. */
  private var streaming = false
  /**
   * Whether the body is past its cap, as the last layout measured it. The header
   * reads it, and a layout that changes it rebuilds the header before laying it
   * out, because a button shown after that would get no second layout pass.
   */
  private var capped = false

  /**
   * The block's ordinal among the document's code blocks, which a toggle is keyed
   * by. It comes from the segment rather than from a count the view walks, so it is
   * the same number the measurement used.
   */
  var ordinal: Int = -1

  /** The fence's parsed meta, so the host resolves the layout the same way. */
  val fenceMeta: FenceMeta get() = segment?.fenceMeta ?: FenceMeta()

  private val copyButton = controlButton(R.drawable.ds_ic_copy, LABEL_COPY_CODE)
  private val downloadButton = controlButton(R.drawable.ds_ic_download, LABEL_DOWNLOAD_FILE)
  private val wrapButton = controlButton(R.drawable.ds_ic_wrap, LABEL_WRAP_CODE)
  private val expandButton = controlButton(R.drawable.ds_ic_expand, LABEL_EXPAND_CODE)

  private val allButtons: List<ImageButton>
    get() = listOf(expandButton, wrapButton, downloadButton, copyButton)

  private val clearCopied = Runnable {
    copied = false
    updateButtons()
  }

  init {
    setWillNotDraw(false)
    addView(header)
    addView(viewport)
    // The label takes the slack, which keeps the buttons at the trailing edge.
    header.addView(language, LinearLayout.LayoutParams(0, LayoutParams.MATCH_PARENT, 1f))
    header.addView(buttons, LinearLayout.LayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.MATCH_PARENT))
    // A fixed order, so the expand button does not shift the others as it appears
    // and disappears with a block's height.
    buttons.addView(expandButton)
    buttons.addView(wrapButton)
    buttons.addView(downloadButton)
    buttons.addView(copyButton)
    viewport.addView(row, LayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.MATCH_PARENT))
    row.addView(gutter, LinearLayout.LayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.MATCH_PARENT))
    row.addView(scroll, LinearLayout.LayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.MATCH_PARENT))
    scroll.addView(body, LayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.MATCH_PARENT))
  }

  private fun controlButton(drawable: Int, label: Int): ImageButton =
    newControlButton(context, drawable).apply {
    contentDescription = controlLabels[label]
    setOnClickListener {
      when (it) {
        wrapButton -> controlHost?.onToggleWrap(ordinal, segment?.language.orEmpty(), !(layout?.wraps ?: false))
        expandButton -> controlHost?.onToggleExpand(ordinal, segment?.language.orEmpty(), !(layout?.expanded ?: false))
        downloadButton -> controlHost?.onControl(
          BLOCK_CODE,
          segment?.blockIndex ?: 0,
          1,
          segment?.language.orEmpty(),
          "download",
          "text",
        )
        else -> {
          markCopied()
          controlHost?.onControl(
            BLOCK_CODE,
            segment?.blockIndex ?: 0,
            1,
            segment?.language.orEmpty(),
            "copy",
            "text",
          )
        }
      }
    }
  }

  fun setControlHost(host: ControlHost?) {
    controlHost = host
  }

  fun setControlLabels(value: List<String>) {
    controlLabels = if (value.size >= DEFAULT_LABELS.size) value else DEFAULT_LABELS
    updateButtons()
  }

  fun setSegment(segment: CodeSegment, rendered: RenderedDocument) {
    this.rendered = rendered
    if (segment === this.segment) return
    // The tail block is rebuilt on every snapshot while its fence is open, so both
    // offsets are kept only while the block still looks like the same one.
    val same = this.segment != null && this.segment?.firstLine == segment.firstLine
    savedOffset = if (same) scroll.scrollX else 0
    savedVerticalOffset = if (same) viewport.scrollY else 0
    restoreScroll = true
    // A new block arrives unsettled, so a capped card starts pinned to its newest
    // line. `setStreaming` turns that off once the fence closes.
    streaming = true
    ordinal = segment.codeOrdinal
    this.segment = segment
    // The card's own fill, and the header row's three colors, all from the
    // theme. Read once per block rather than per draw, because the card is
    // redrawn whenever its bounds change.
    cardBackground.setColor(segment.colors.codeBlockBackground)
    cardBackground.cornerRadius = segment.radius
    // A theme that sets no border leaves the card unfilled at its edge, which is
    // a different thing from a border of zero width in one color.
    val border = segment.colors.slotOrNull(ThemeSlots.SLOT_CODE_BLOCK_BORDER_COLOR)
    if (segment.borderWidth > 0f && border != null) {
      cardBackground.setStroke(segment.borderWidth.toInt(), border)
    } else {
      cardBackground.setStroke(0, 0)
    }
    headerBackground = segment.colors.headerBackground
    headerBorder = segment.colors.headerBorderColor
    language.setTextColor(segment.colors.headerColor)
    language.setTextSize(TypedValue.COMPLEX_UNIT_PX, segment.headerSize)
    language.typeface = segment.headerFace
    // A selected button takes the text color, so its state is visible. The buttons
    // take the theme's own button color where it gave them one.
    val tint = ColorStateList(
      arrayOf(intArrayOf(android.R.attr.state_selected), intArrayOf()),
      intArrayOf(segment.colors.text, segment.colors.headerButtonColor),
    )
    for (button in allButtons) button.imageTintList = tint
    body.setContent(segment.text, segment.accessibleBlocks, rendered)
    // A fence's title replaces the language, and the spoken label keeps both plus
    // the start line, so a screen reader still hears what kind of code this is.
    language.text = if (segment.title.isNotEmpty()) segment.title else segment.language
    language.contentDescription = spokenLabel(segment)
    updateButtons()
    requestLayout()
    invalidate()
  }

  /**
   * "src/app.ts, ts code, from line 10": both names, and the start line when it is
   * not the default. The numbers themselves are never read aloud.
   */
  private fun spokenLabel(segment: CodeSegment): String {
    val parts = ArrayList<String>(3)
    if (segment.title.isNotEmpty() && segment.title != segment.language) parts.add(segment.title)
    parts.add(if (segment.language.isNotEmpty()) "${segment.language} code" else "code")
    if (segment.fenceMeta.startLine != 1) parts.add("from line ${segment.fenceMeta.startLine}")
    return parts.joinToString(", ")
  }

  fun setControls(controls: Int) {
    if (controls == this.controls) return
    this.controls = controls
    updateButtons()
  }

  fun setControlsEnabled(enabled: Boolean) {
    if (enabled == controlsEnabled) return
    controlsEnabled = enabled
    updateButtons()
  }

  /**
   * The block's resolved layout, from the props, the reader's choices, and the
   * fence's own meta. The same value the measurement used, so a measured card and
   * a drawn card are the same size.
   */
  fun setLayout(layout: CodeBlockLayout) {
    if (this.layout == layout) return
    this.layout = layout
    updateButtons()
    requestLayout()
  }

  /** False once the block's fence has closed, which stops a capped card following. */
  fun setStreaming(streaming: Boolean) {
    if (streaming == this.streaming) return
    this.streaming = streaming
    viewport.streaming = streaming
    requestLayout()
  }

  /** Drops the saved offsets and returns to the top, for a new generation. */
  fun resetScroll() {
    savedOffset = 0
    savedVerticalOffset = 0
    requestLayout()
  }

  fun setHeaderVisible(visible: Boolean) {
    val next = if (visible) View.VISIBLE else View.GONE
    if (header.visibility == next) return
    header.visibility = next
    updateButtons()
    requestLayout()
  }

  private fun updateButtons() {
    val shown = header.visibility == View.VISIBLE
    buttons.visibility = if (shown) View.VISIBLE else View.GONE
    language.visibility = if (shown) View.VISIBLE else View.GONE
    if (!shown) return
    val layout = this.layout
    val expanded = layout?.expanded ?: false
    // Expand appears only on a block past the cap, because that is the only block
    // it can do anything for. An expanded block keeps it as a collapse button, so
    // the cap can come back.
    expandButton.visibility =
      if (controls and CONTROL_CODE_EXPAND != 0 && (capped || expanded)) View.VISIBLE else View.GONE
    wrapButton.visibility = if (controls and CONTROL_CODE_WRAP != 0) View.VISIBLE else View.GONE
    downloadButton.visibility = if (controls and CONTROL_CODE_DOWNLOAD != 0) View.VISIBLE else View.GONE
    copyButton.visibility = if (controls and CONTROL_CODE_COPY != 0) View.VISIBLE else View.GONE
    for (button in allButtons) {
      button.isEnabled = controlsEnabled && button.visibility == View.VISIBLE
      button.alpha = if (button.isEnabled) 1f else 0.4f
    }
    copyButton.setImageResource(if (copied) R.drawable.ds_ic_check else R.drawable.ds_ic_copy)
    copyButton.contentDescription = controlLabels[if (copied) LABEL_COPIED else LABEL_COPY_CODE]
    val wraps = layout?.wraps ?: false
    wrapButton.isSelected = wraps
    wrapButton.contentDescription = controlLabels[if (wraps) LABEL_UNWRAP_CODE else LABEL_WRAP_CODE]
    wrapButton.stateDescription = wrapButton.contentDescription
    expandButton.isSelected = expanded
    expandButton.setImageResource(if (expanded) R.drawable.ds_ic_collapse else R.drawable.ds_ic_expand)
    expandButton.contentDescription =
      controlLabels[if (expanded) LABEL_COLLAPSE_CODE else LABEL_EXPAND_CODE]
    expandButton.stateDescription = expandButton.contentDescription
  }

  private fun markCopied() {
    copied = true
    host.removeCallbacks(clearCopied)
    host.postDelayed(clearCopied, COPIED_DURATION_MS)
    ControlDelivery.announceCopied(copyButton, controlLabels[LABEL_COPIED])
    updateButtons()
  }

  /**
   * The header row's height: the one the measurement reserved, not the header's
   * own measured height, which as a frame child would be the whole card.
   */
  private fun headerHeight(): Int {
    val rendered = this.rendered ?: return 0
    return if (header.visibility == View.VISIBLE) rendered.style.chromeRowHeight else 0
  }

  override fun onLayout(changed: Boolean, left: Int, top: Int, right: Int, bottom: Int) {
    val segment = this.segment ?: return
    val rendered = this.rendered ?: return
    val layout = this.layout ?: return
    // Children are placed in this view's own coordinates, so the frame the parent
    // gave (left, top) only supplies the size.
    val width = right - left
    val height = bottom - top
    val headerHeight = headerHeight()
    val padding = segment.padding
    val leading = segment.contentLeading
    val cardWidth = (width - leading).coerceAtLeast(0)
    val gutterWidth = segment.gutterWidth(layout)
    gutter.visibility = if (gutterWidth > 0) View.VISIBLE else View.GONE
    // The gutter narrows the width a wrapped body wraps at, which is the only way
    // numbers change a block's height. An unwrapped body is laid out at its own
    // length either way, so it never lays code out again on a rotation.
    val bodyWidth = segment.bodyWidth(rendered.paint, cardWidth, layout.wraps, gutterWidth)
    val bodyHeight = segment.bodyHeight(rendered.paint, cardWidth, layout.wraps, gutterWidth)
    // The viewport scrolls only while the block is past its cap, which is the whole
    // difference between a capped card and an uncapped one. It is decided before
    // the header is laid out, because the expand button depends on it.
    val capped = layout.maxHeight > 0f && bodyHeight + 2 * padding > layout.maxHeight
    if (capped != this.capped) {
      this.capped = capped
      updateButtons()
    }
    header.measure(
      MeasureSpec.makeMeasureSpec(width, MeasureSpec.EXACTLY),
      MeasureSpec.makeMeasureSpec(headerHeight, MeasureSpec.EXACTLY),
    )
    header.layout(0, 0, width, headerHeight)
    val viewportLeft = leading + padding
    val viewportTop = headerHeight + padding
    val viewportRight = (width - padding).coerceAtLeast(viewportLeft)
    val viewportBottom = (height - padding).coerceAtLeast(viewportTop)
    viewport.measure(
      MeasureSpec.makeMeasureSpec(viewportRight - viewportLeft, MeasureSpec.EXACTLY),
      MeasureSpec.makeMeasureSpec(viewportBottom - viewportTop, MeasureSpec.EXACTLY),
    )
    viewport.layout(viewportLeft, viewportTop, viewportRight, viewportBottom)
    // The row is as tall as the code, so a line's y in the gutter is its y in the
    // body's layout and the numbers beside the visible lines are the visible ones.
    row.measure(
      MeasureSpec.makeMeasureSpec(viewport.width, MeasureSpec.AT_MOST),
      MeasureSpec.makeMeasureSpec(bodyHeight, MeasureSpec.EXACTLY),
    )
    row.layout(0, 0, row.measuredWidth, bodyHeight)
    gutter.measure(
      MeasureSpec.makeMeasureSpec(gutterWidth, MeasureSpec.EXACTLY),
      MeasureSpec.makeMeasureSpec(bodyHeight, MeasureSpec.EXACTLY),
    )
    gutter.layout(0, 0, gutterWidth, bodyHeight)
    scroll.measure(
      MeasureSpec.makeMeasureSpec(bodyWidth.coerceAtLeast(0), MeasureSpec.EXACTLY),
      MeasureSpec.makeMeasureSpec(bodyHeight, MeasureSpec.EXACTLY),
    )
    scroll.layout(gutterWidth, 0, gutterWidth + bodyWidth, bodyHeight)
    body.measure(
      MeasureSpec.makeMeasureSpec(bodyWidth, MeasureSpec.EXACTLY),
      MeasureSpec.makeMeasureSpec(bodyHeight, MeasureSpec.EXACTLY),
    )
    body.layout(0, 0, bodyWidth, bodyHeight)
    gutter.update(
      segment, body, layout, segment.codeTextSize, segment.codeFace,
      lineNumberColor(segment.colors),
    )
    viewport.isVerticalScrollBarEnabled = capped
    viewport.streaming = streaming
    if (restoreScroll) {
      val maximum = (bodyWidth - scroll.width).coerceAtLeast(0)
      scroll.scrollTo(savedOffset.coerceIn(0, maximum), 0)
      val verticalMaximum = (bodyHeight - viewport.height).coerceAtLeast(0)
      viewport.scrollTo(0, savedVerticalOffset.coerceIn(0, verticalMaximum))
      restoreScroll = false
    }
    // Follow the newest line, which is Streamdown's pinned scroll: a card the
    // reader has scrolled away from is not pinned and does not move. Not an
    // alternative to the restore above: an open fence is rebuilt on every
    // snapshot, so the restore runs every time, and the pin has to land on top.
    if (capped && streaming) {
      viewport.followNewestLine()
    }
    invalidate()
  }

  /**
   * The code's own color, dimmed, which is what the numbers draw in.
   *
   * They annotate the code beside them rather than styling themselves, so a theme
   * that restyles `codeBlock.color` restyles its line numbers in step. This is
   * Streamdown's `muted-foreground/50` gutter expressed against the theme's own
   * code color rather than a fixed gray. Mirrors `kGutterAlpha` on iOS.
   */
  private fun lineNumberColor(colors: Palette): Int {
    val base = colors.codeText
    val alpha = ((base ushr 24) * GUTTER_ALPHA).toInt().coerceIn(0, 255)
    return (alpha shl 24) or (base and 0x00FFFFFF)
  }

  override fun onDetachedFromWindow() {
    host.removeCallbacks(clearCopied)
    super.onDetachedFromWindow()
  }

  // The card's rounded code background, plus the container bars and the list
  // marker, which live in the non-scrolling margin beside the body.
  override fun onDraw(canvas: Canvas) {
    super.onDraw(canvas)
    val segment = this.segment ?: return
    val headerHeight = headerHeight()
    // The card starts below the header, which sits on the page, as on iOS.
    cardBackground.setBounds(segment.contentLeading, headerHeight, width, height)
    cardBackground.draw(canvas)
    // The header row's own fill and rule, drawn under its buttons and label.
    segment.colors.headerBackground?.let { fill ->
      barPaint.color = fill
      canvas.drawRect(0f, 0f, width.toFloat(), headerHeight.toFloat(), barPaint)
    }
    if (segment.colors.headerBorderColor != null) {
      barPaint.color = segment.colors.headerBorderColor
      canvas.drawRect(0f, headerHeight - 1f, width.toFloat(), headerHeight.toFloat(), barPaint)
    }
    val firstLine = segment.accessibleBlocks.firstOrNull()
    for (container in segment.containers) {
      val bar = container.barColor
      // A zero-width bar is a bar the theme removed.
      if (bar != null && container.barWidth > 0f) {
        barPaint.color = bar
        val x = container.leading + 2f
        canvas.drawRect(x, headerHeight.toFloat(), x + container.barWidth, height.toFloat(), barPaint)
      }
      val marker = container.marker
      if (marker != null && firstLine != null) {
        // The marker's own family and weight, at the size the theme sets.
        val markerPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
          typeface = segment.markerFace
          textSize = segment.markerSize
        }
        val baseline = headerHeight + segment.padding + markerPaint.fontMetrics.ascent
        val x = container.leading + container.width - container.markerGap - markerPaint.measureText(marker)
        canvas.drawText(marker, x, baseline.toFloat(), markerPaint)
      }
    }
  }
}

/**
 * One table: fixed column widths in a horizontal scroll view, a toolbar above
 * the grid, and container bars and the list marker in the leading margin.
 */
internal class TableSegmentView(context: Context) : FrameLayout(context) {
  private val scroll = HorizontalScrollView(context).apply {
    isHorizontalScrollBarEnabled = true
    isFillViewport = false
    overScrollMode = OVER_SCROLL_NEVER
  }
  private val grid = TableGridView(context)
  private val paint = Paint(Paint.ANTI_ALIAS_FLAG)
  private val toolbar = LinearLayout(context).apply {
    orientation = LinearLayout.HORIZONTAL
    gravity = Gravity.END or Gravity.CENTER_VERTICAL
  }
  private val copyButton = newControlButton(context, R.drawable.ds_ic_copy)
  private val downloadButton = newControlButton(context, R.drawable.ds_ic_download)
  private var segment: TableSegment? = null
  private var sourceEpoch = -1
  private var pendingLeadingDistance: Int? = 0
  private var controlLabels: List<String> = DEFAULT_LABELS
  private var controls = 0
  private var controlsEnabled = false
  private var controlHost: ControlHost? = null
  /** The toolbar row the measurement reserved, from the document's style. */
  private var chromeRowHeight = 0

  init {
    setWillNotDraw(false)
    scroll.addView(grid, LayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.MATCH_PARENT))
    addView(toolbar)
    addView(scroll)
    toolbar.addView(copyButton)
    toolbar.addView(downloadButton)
    copyButton.setOnClickListener { showFormats(it, "copy") }
    downloadButton.setOnClickListener { showFormats(it, "download") }
  }

  fun setControlHost(host: ControlHost?) {
    controlHost = host
  }

  fun setControlLabels(value: List<String>) {
    controlLabels = if (value.size >= DEFAULT_LABELS.size) value else DEFAULT_LABELS
    updateButtons()
  }

  fun setControls(controls: Int) {
    if (controls == this.controls) return
    this.controls = controls
    updateButtons()
  }

  fun setControlsEnabled(enabled: Boolean) {
    if (enabled == controlsEnabled) return
    controlsEnabled = enabled
    updateButtons()
  }

  fun setToolbarVisible(visible: Boolean) {
    val next = if (visible) View.VISIBLE else View.GONE
    if (toolbar.visibility == next) return
    toolbar.visibility = next
    updateButtons()
    requestLayout()
  }

  private fun updateButtons() {
    val shown = toolbar.visibility == View.VISIBLE
    copyButton.visibility = if (shown && controls and CONTROL_TABLE_COPY != 0) View.VISIBLE else View.GONE
    downloadButton.visibility =
      if (shown && controls and CONTROL_TABLE_DOWNLOAD != 0) View.VISIBLE else View.GONE
    for (button in listOf(copyButton, downloadButton)) {
      button.isEnabled = controlsEnabled && button.visibility == View.VISIBLE
      button.alpha = if (button.isEnabled) 1f else 0.4f
    }
    copyButton.contentDescription = controlLabels[LABEL_COPY_TABLE]
    downloadButton.contentDescription = controlLabels[LABEL_DOWNLOAD_TABLE]
  }

  /**
   * The formats a table can be copied or saved in. Every format also goes on
   * the clipboard as HTML, so a paste lands as a table rather than as text.
   */
  private fun showFormats(anchor: View, action: String) {
    val segment = this.segment ?: return
    val formats = if (action == "copy") {
      listOf(
        LABEL_FORMAT_MARKDOWN to "markdown",
        LABEL_FORMAT_CSV to "csv",
        LABEL_FORMAT_TSV to "tsv",
      )
    } else {
      listOf(LABEL_FORMAT_CSV to "csv", LABEL_FORMAT_MARKDOWN to "markdown")
    }
    val menu = PopupMenu(context, anchor)
    for ((label, format) in formats) {
      menu.menu.add(0, 0, menu.menu.size(), controlLabels[label]).setOnMenuItemClickListener {
        controlHost?.onControl(
          BLOCK_TABLE_ROW,
          segment.blockIndex,
          segment.blockCount,
          "",
          action,
          format,
        )
        true
      }
    }
    menu.show()
  }

  fun setSegment(segment: TableSegment, sourceEpoch: Int, chromeRowHeight: Int) {
    if (chromeRowHeight != this.chromeRowHeight) {
      this.chromeRowHeight = chromeRowHeight
      requestLayout()
    }
    if (segment === this.segment) {
      return
    }
    val previous = this.segment
    val sameTable = this.sourceEpoch == sourceEpoch && previous != null &&
      previous.rtl == segment.rtl &&
      previous.accessibilityRows.firstOrNull() == segment.accessibilityRows.firstOrNull() &&
      (previous.identity == segment.identity || previous.identity == 0L || segment.identity == 0L)
    val previousRange = (previous?.tableWidth ?: 0).minus(scroll.width).coerceAtLeast(0)
    pendingLeadingDistance = if (sameTable) {
      pendingLeadingDistance ?: if (segment.rtl) (previousRange - scroll.scrollX).coerceAtLeast(0) else scroll.scrollX
    } else {
      0
    }
    this.segment = segment
    this.sourceEpoch = sourceEpoch
    val tint = ColorStateList.valueOf(segment.colors.toolbarButtonColor)
    copyButton.imageTintList = tint
    downloadButton.imageTintList = tint
    grid.updateSegment(segment, resetFocus = !sameTable)
    requestLayout()
    invalidate()
  }

  fun resetScroll() {
    sourceEpoch = -1
    pendingLeadingDistance = 0
    requestLayout()
  }

  private fun toolbarHeight(): Int = if (toolbar.visibility == View.VISIBLE) chromeRowHeight else 0

  override fun onLayout(changed: Boolean, left: Int, top: Int, right: Int, bottom: Int) {
    val segment = segment ?: return
    // In this view's own coordinates, and at the reserved row height rather than
    // the toolbar's measured one, which as a frame child is the whole table.
    val toolbarHeight = toolbarHeight()
    toolbar.measure(
      MeasureSpec.makeMeasureSpec(right - left, MeasureSpec.EXACTLY),
      MeasureSpec.makeMeasureSpec(toolbarHeight, MeasureSpec.EXACTLY),
    )
    toolbar.layout(0, 0, right - left, toolbarHeight)
    val leading = segment.contentLeading.toInt()
    val width = (right - left - leading).coerceAtLeast(0)
    val height = (bottom - top - toolbarHeight).coerceAtLeast(0)
    val oldRange = (segment.tableWidth - scroll.width).coerceAtLeast(0)
    val distance = pendingLeadingDistance ?: if (segment.rtl) {
      (oldRange - scroll.scrollX).coerceAtLeast(0)
    } else {
      scroll.scrollX
    }
    scroll.measure(
      MeasureSpec.makeMeasureSpec(width, MeasureSpec.EXACTLY),
      MeasureSpec.makeMeasureSpec(height, MeasureSpec.EXACTLY),
    )
    // A toolbar above the grid takes layout space rather than floating over
    // the rows, so a reader never taps a button instead of a cell.
    val scrollLeft = (if (segment.rtl) 0 else leading) + 0
    val scrollTop = toolbarHeight
    scroll.layout(scrollLeft, scrollTop, scrollLeft + width, scrollTop + height)
    grid.measure(
      MeasureSpec.makeMeasureSpec(segment.tableWidth, MeasureSpec.EXACTLY),
      MeasureSpec.makeMeasureSpec(segment.tableHeight, MeasureSpec.EXACTLY),
    )
    grid.layout(0, 0, segment.tableWidth, segment.tableHeight)
    val newRange = (segment.tableWidth - width).coerceAtLeast(0)
    scroll.scrollTo((if (segment.rtl) newRange - distance else distance).coerceIn(0, newRange), 0)
    pendingLeadingDistance = null
  }

  override fun onDraw(canvas: Canvas) {
    super.onDraw(canvas)
    val segment = segment ?: return
    val density = segment.density
    val toolbarHeight = toolbarHeight()
    for (container in segment.containers) {
      val barColor = container.barColor
      if (barColor != null) {
        paint.color = barColor
        val x = if (segment.rtl) width - container.leading - 5 * density
          else container.leading + 2 * density
        canvas.drawRect(x, toolbarHeight.toFloat(), x + 3 * density, height.toFloat(), paint)
      }
      val marker = container.marker
      if (marker != null) {
        val firstCell = segment.cells.firstOrNull()?.firstOrNull() ?: continue
        val baseline = toolbarHeight + segment.paddingVertical + firstCell.getLineBaseline(0).toFloat()
        val x = if (segment.rtl) width - container.leading - container.width + 6 * density
          else container.leading + container.width - 6 * density - segment.markerPaint.measureText(marker)
        canvas.drawText(marker, x, baseline, segment.markerPaint)
      }
    }
  }
}

private class TableReadingPart(
  val text: String,
  val row: Int,
  val column: Int?,
  val piece: AccessibleTextPiece?,
  val heading: Boolean,
)

private class TableGridView(context: Context) : View(context) {
  private var readingParts = emptyList<TableReadingPart>()
  private val accessibility = MarkdownAccessibility(this, { readingParts.size }) { accessiblePart(it) }
  private var segment: TableSegment? = null
  private val paint = Paint()
  private var pressedLink = -1

  fun updateSegment(segment: TableSegment, resetFocus: Boolean) {
    this.segment = segment
    readingParts = buildReadingParts(segment)
    invalidate()
    accessibility.contentChanged(resetFocus)
  }

  override fun getAccessibilityNodeProvider(): AccessibilityNodeProvider = accessibility

  private fun buildReadingParts(segment: TableSegment): List<TableReadingPart> = buildList {
    segment.cells.forEachIndexed { row, cells ->
      val plain = StringBuilder()
      var first = true
      fun appendPart(value: String, column: Int?, piece: AccessibleTextPiece?) {
        val heading = first && segment.hasHeader && row == 0
        add(TableReadingPart(if (heading) "Header: $value" else value, row, column, piece, heading))
        first = false
      }
      fun flushPlain() {
        if (plain.isNotEmpty()) {
          appendPart(plain.toString(), null, null)
          plain.clear()
        }
      }
      cells.forEachIndexed { column, cell ->
        if (column > 0) plain.append(", ")
        val text = cell.text
        if (text is Spanned) {
          for (piece in linkPieces(text, 0, text.length)) {
            val value = text.subSequence(piece.start, piece.end).toString()
            if (piece.linkIndex == null) {
              plain.append(value)
            } else {
              flushPlain()
              appendPart(value, column, piece)
            }
          }
        } else {
          plain.append(text)
        }
      }
      flushPlain()
      if (first) appendPart("", null, null)
    }
  }

  private fun accessiblePart(index: Int): MarkdownAccessibility.Entry? {
    val segment = segment ?: return null
    val part = readingParts.getOrNull(index) ?: return null
    val top = segment.rowTops[part.row]
    val bottom = segment.rowTops[part.row + 1]
    val bounds = Rect(0, top, segment.tableWidth, bottom)
    val column = part.column
    val piece = part.piece
    if (column != null && piece != null) {
      val cell = segment.cells[part.row][column]
      val selectionPath = Path()
      cell.getSelectionPath(piece.start, piece.end, selectionPath)
      val selectedBounds = RectF()
      selectionPath.computeBounds(selectedBounds, true)
      if (!selectedBounds.isEmpty) {
        selectedBounds.roundOut(bounds)
        val (x, y) = cellOrigin(segment, part.row, column)
        bounds.offset(x, y)
      }
    }
    val link = piece?.linkIndex
    return MarkdownAccessibility.Entry(
      text = part.text,
      bounds = bounds,
      heading = part.heading,
      linkPress = if (link == null) null else ({
        var parent = parent
        while (parent != null && parent !is DownstreamMarkdownView) parent = parent.parent
        (parent as? DownstreamMarkdownView)?.pressLink(link)
      }),
    )
  }

  private fun cellOrigin(segment: TableSegment, row: Int, column: Int): Pair<Int, Int> {
    return Pair(segment.columnLefts[column] + segment.paddingHorizontal,
      segment.rowTops[row] + segment.paddingVertical)
  }

  override fun onDraw(canvas: Canvas) {
    val segment = segment ?: return
    val hairline = segment.borderWidth.coerceAtLeast(1)
    val width = segment.tableWidth.toFloat() - hairline
    if (segment.hasHeader && segment.rowHeights.isNotEmpty()) {
      // The header row's own fill, which a theme can set apart from the body.
      paint.color = segment.colors.tableHeaderBackground
      canvas.drawRect(0f, 0f, width, segment.rowHeights[0].toFloat(), paint)
    }
    segment.cells.forEachIndexed { row, cells ->
      // The zebra fill is drawn under the cells, per row rather than per cell,
      // so an empty trailing cell in a short row still gets the row's fill.
      if (row >= if (segment.hasHeader) 1 else 0) {
        val stripe = segment.colors.rowStripe(row - if (segment.hasHeader) 1 else 0)
        if (stripe != null) {
          paint.color = stripe
          canvas.drawRect(
            0f,
            segment.rowTops[row].toFloat(),
            width,
            segment.rowTops[row + 1].toFloat(),
            paint,
          )
        }
      }
      cells.forEachIndexed { column, cell ->
        val (x, y) = cellOrigin(segment, row, column)
        canvas.save()
        canvas.translate(x.toFloat(), y.toFloat())
        cell.draw(canvas)
        canvas.restore()
      }
    }
    paint.color = segment.colors.separator
    var y = 0f
    canvas.drawRect(0f, 0f, width, hairline.toFloat(), paint)
    for (height in segment.rowHeights) {
      y += height
      canvas.drawRect(0f, y - hairline, width, y, paint)
    }
    var x = if (segment.rtl) width else 0f
    canvas.drawRect(x, 0f, x + hairline, y, paint)
    for (columnWidth in segment.columnWidths) {
      x += if (segment.rtl) -columnWidth else columnWidth
      canvas.drawRect(x, 0f, x + hairline, y, paint)
    }
  }

  private fun linkAtPoint(px: Float, py: Float): Int {
    val segment = segment ?: return -1
    segment.cells.forEachIndexed { row, cells ->
      cells.forEachIndexed { column, cell ->
        val (x, y) = cellOrigin(segment, row, column)
        val right = x + segment.columnWidths[column] - 2 * segment.paddingHorizontal
        val bottom = y + segment.rowHeights[row] - 2 * segment.paddingVertical
        if (px >= x - HIT_SLOP && px <= right + HIT_SLOP && py >= y - HIT_SLOP && py <= bottom + HIT_SLOP) {
          return linkAt(cell, px - x, py - y)
        }
      }
    }
    return -1
  }

  @SuppressLint("ClickableViewAccessibility")
  override fun onTouchEvent(event: MotionEvent): Boolean {
    when (event.actionMasked) {
      MotionEvent.ACTION_DOWN -> {
        pressedLink = linkAtPoint(event.x, event.y)
        return pressedLink >= 0
      }
      MotionEvent.ACTION_UP -> {
        val link = pressedLink
        pressedLink = -1
        if (link >= 0 && linkAtPoint(event.x, event.y) == link) {
          var parent = parent
          while (parent != null && parent !is DownstreamMarkdownView) parent = parent.parent
          (parent as? DownstreamMarkdownView)?.pressLink(link)
          return true
        }
      }
      MotionEvent.ACTION_CANCEL -> pressedLink = -1
    }
    return pressedLink >= 0
  }
}
