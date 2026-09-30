package com.downstream

import android.annotation.SuppressLint
import android.content.Context
import android.content.res.Configuration
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
import android.widget.TextView
import android.view.accessibility.AccessibilityNodeInfo
import android.view.accessibility.AccessibilityNodeProvider
import java.lang.ref.WeakReference
import com.facebook.react.bridge.Arguments
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

// How long a copy button shows its check before going back, as in Streamdown.
private const val COPIED_DURATION_MS = 2_000L

// Indexes into the label array `src/controls.ts` sends, in the same order.
private val DEFAULT_LABELS = listOf(
  "Copy Code", "Download file", "Copied", "Wrap lines", "Don't wrap lines",
  "Copy table", "Copy table as Markdown", "Copy table as CSV", "Copy table as TSV",
  "Download table", "Download table as CSV", "Download table as Markdown",
  "Markdown", "CSV", "TSV",
)
private const val LABEL_COPY_CODE = 0
private const val LABEL_DOWNLOAD_FILE = 1
private const val LABEL_COPIED = 2
private const val LABEL_WRAP_CODE = 3
private const val LABEL_UNWRAP_CODE = 4
private const val LABEL_COPY_TABLE = 5
private const val LABEL_DOWNLOAD_TABLE = 9
private const val LABEL_FORMAT_MARKDOWN = 12
private const val LABEL_FORMAT_CSV = 13
private const val LABEL_FORMAT_TSV = 14

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
  fun onControlFailed(kind: Int, action: String, message: String)
}

// A 28dp glyph in a 48dp target, which is the smallest comfortable touch
// target the platform asks for.
private val GLYPH_SIZE = 28
private val TOUCH_SIZE = 48

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
      filename: String) -> Unit)? = null
  internal var onControlFailure:
    ((view: DownstreamMarkdownView, block: String, action: String, message: String) -> Unit)? = null
  internal var onWrapToggle: ((view: DownstreamMarkdownView, wrapped: Boolean) -> Unit)? = null

  private var documentId = 0L
  private var fontScale = 1f
  private var rendered: RenderedDocument? = null
  private var selectable = true
  private var syntaxHighlighting = true
  private var controls = CONTROL_CODE_COPY or CONTROL_CODE_DOWNLOAD or CONTROL_TABLE_COPY or
    CONTROL_TABLE_DOWNLOAD or CONTROL_CODE_WRAP
  private var controlLabels: List<String> = DEFAULT_LABELS
  private var chrome = 0
  private var codeBlockWrap = false
  private var codeWrapToggles = emptySet<Int>()
  /**
   * `codeWrapToggles` in the ascending form `RenderedDocument.codeWraps` and the
   * measurement both read, so the frame a card is given and the layout it draws
   * inside it are one decision.
   */
  private var wrapToggleOrdinals = IntArray(0)
  private var csvSeparator = ","
  private var codeFilename = "file"
  private var tableFilename = "table"
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
    for (index in 0 until childCount) {
      (getChildAt(index) as? TableSegmentView)?.resetScroll()
    }
  }

  fun setDocument(documentId: Long, fontScale: Float) {
    if (documentId == this.documentId && fontScale == this.fontScale && rendered != null) {
      return
    }
    if (pendingTraceDocumentId != 0L && pendingTraceDocumentId != documentId) {
      DownstreamNative.traceDocument(pendingTraceDocumentId, "frame_unavailable")
      pendingTraceDocumentId = 0L
    }
    this.documentId = documentId
    this.fontScale = fontScale
    val tracing = documentId != 0L && DownstreamNative.isTracingDocument(documentId)
    if (tracing) DownstreamNative.traceDocument(documentId, "apply_start")
    rendered = if (documentId != 0L) {
      RenderedDocument.obtain(context, documentId, fontScale, syntaxHighlighting)
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
      }
    }
  }

  fun setCodeFilename(value: String) {
    codeFilename = value
  }

  fun setTableFilename(value: String) {
    tableFilename = value
  }

  fun setCsvSeparator(value: String) {
    csvSeparator = value
  }

  fun setCodeBlockWrap(wrap: Boolean) {
    if (wrap == codeBlockWrap) return
    codeBlockWrap = wrap
    applySettledState()
  }

  fun setChrome(value: Int) {
    if (value == chrome) return
    chrome = value
    applySettledState()
  }

  fun setCodeWrapToggles(toggles: Set<Int>) {
    codeWrapToggles = toggles
    wrapToggleOrdinals = toggles.sorted().toIntArray()
    applySettledState()
    requestLayout()
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
          // The same helper the measurement used, so the frame this card is
          // given and the layout it draws inside it are one decision.
          val ordinal = child.ordinal
          child.setControls(controls)
          child.setControlsEnabled(settled)
          child.setWrapped(
            if (ordinal >= 0) {
              RenderedDocument.codeWraps(codeBlockWrap, wrapToggleOrdinals, ordinal)
            } else {
              codeBlockWrap
            },
          )
          child.setHeaderVisible(chrome and RenderedDocument.CHROME_CODE_HEADER != 0)
        }
        is TableSegmentView -> {
          child.setControls(controls)
          child.setControlsEnabled(settled)
          child.setToolbarVisible(chrome and RenderedDocument.CHROME_TABLE_TOOLBAR != 0)
        }
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
      onControl?.invoke(this, block, request.action, request.format, request.language, "")
      return
    }
    val weakView = WeakReference(this)
    exporter.save(request.filename, request.mime, bytes ?: ByteArray(0)) { url, failure ->
      val view = weakView.get() ?: return@save
      if (!view.acceptsControlExport(request)) return@save
      if (failure != null) {
        view.onControlFailure?.invoke(view, block, "download", failure)
      } else if (url != null) {
        view.onControl?.invoke(view, block, request.action, request.format, request.language, request.filename)
      }
    }
  }

  override fun onToggleWrap(ordinal: Int, language: String, wrapped: Boolean) {
    nextWrappedOrdinal = ordinal
    onWrapToggle?.invoke(this, wrapped)
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
        rendered = RenderedDocument.obtain(context, documentId, fontScale, syntaxHighlighting)
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
    rendered = null
    syntaxHighlighting = true
    generation = 0
    sessionId = null
    sourceEpoch++
    onControl = null
    onControlFailure = null
    onWrapToggle = null
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
   * The ordinal of the code block whose wrap button was pressed, or -1 when the
   * view cannot tell which one it was. -1 happens after a recycle, where a
   * toggle would name a block in a document that is no longer mounted.
   */
  fun nextUnwrappedOrdinal(): Int {
    val toggled = nextWrappedOrdinal
    if (toggled < 0) return -1
    nextWrappedOrdinal = -1
    return toggled
  }

  private var nextWrappedOrdinal = -1

  /** Hands a new state back with `ordinal` toggled. */
  fun toggleWrap(ordinal: Int) {
    val wrapper = stateWrapper ?: return
    val toggles = codeWrapToggles.toMutableSet()
    if (!toggles.add(ordinal)) {
      toggles.remove(ordinal)
    }
    val map = Arguments.createMap()
    map.putDouble("documentId", documentId.toDouble())
    map.putDouble("fontScale", fontScale.toDouble())
    map.putDouble("chrome", chromeForToggles(toggles).toDouble())
    val array = Arguments.createArray()
    for (value in toggles) {
      array.pushInt(value)
    }
    map.putArray("wrapToggles", array)
    // The prop's own setting travels beside the set, because together they
    // decide each block's layout and neither half can be inferred from chrome.
    map.putDouble("codeBlockWrap", if (codeBlockWrap) 1.0 else 0.0)
    codeWrapToggles = toggles
    wrapToggleOrdinals = toggles.sorted().toIntArray()
    chrome = chromeForToggles(toggles)
    applySettledState()
    requestLayout()
    wrapper.updateState(map)
  }

  /**
   * The chrome for a set of toggles, with the same rule the shadow node uses:
   * a header and a toolbar are bits, and the wrap bit says every code block
   * wraps, which stops being true as soon as the reader has made one block
   * differ from the prop. A mixed document's per-block layout comes from
   * `codeWraps`, not from a bit.
   */
  private fun chromeForToggles(toggles: Set<Int>): Int {
    var chrome = 0
    if (controls and (CONTROL_CODE_COPY or CONTROL_CODE_DOWNLOAD or CONTROL_CODE_WRAP) != 0) {
      chrome = chrome or RenderedDocument.CHROME_CODE_HEADER
    }
    if (controls and (CONTROL_TABLE_COPY or CONTROL_TABLE_DOWNLOAD) != 0) {
      chrome = chrome or RenderedDocument.CHROME_TABLE_TOOLBAR
    }
    if (controls and CONTROL_CODE_WRAP != 0 && codeBlockWrap && toggles.isEmpty()) {
      chrome = chrome or RenderedDocument.CHROME_CODE_WRAP
    }
    return chrome
  }

  override fun onConfigurationChanged(newConfig: Configuration) {
    super.onConfigurationChanged(newConfig)
    if (documentId != 0L) {
      RenderedDocument.obtain(context, documentId, fontScale, syntaxHighlighting)?.let {
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
    val segments = rendered?.segments.orEmpty()
    segments.forEachIndexed { index, segment ->
      var view = if (index < childCount) getChildAt(index) else null
      val reusable = when (segment) {
        is TextSegment -> view is TextSegmentView
        is CodeSegment -> view is CodeSegmentView
        is TableSegment -> view is TableSegmentView
      }
      if (!reusable) {
        if (view != null) removeViewAt(index)
        view = when (segment) {
          is TextSegment -> TextSegmentView(context)
          is CodeSegment -> CodeSegmentView(context).apply { setControlHost(this@DownstreamMarkdownView) }
          is TableSegment -> TableSegmentView(context).apply { setControlHost(this@DownstreamMarkdownView) }
        }
        addView(view, index)
      }
      when (segment) {
        is TextSegment -> (view as TextSegmentView).setSegment(segment, rendered!!)
        is CodeSegment -> (view as CodeSegmentView).setSegment(segment, rendered!!)
        is TableSegment -> (view as TableSegmentView).setSegment(segment, sourceEpoch)
      }
    }
    while (childCount > segments.size) {
      removeViewAt(childCount - 1)
    }
    applySettledState()
    layoutSegments()
  }

  // React Native does not relayout children that request it, so segments are
  // laid out here whenever the document or size changes.
  private fun layoutSegments() {
    val document = rendered ?: return
    val width = width - paddingLeft - paddingRight
    if (width <= 0) {
      return
    }
    val frames = document.frames(width, chrome, codeBlockWrap, wrapToggleOrdinals)
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
 * One code block: a fixed header row above a horizontally scrolling body. The
 * body is a text segment view, so selection, plain-text copy, token colors, and
 * link hit testing inside the code are the ones a text segment already has.
 */
internal class CodeSegmentView(context: Context) : FrameLayout(context) {
  private val header = LinearLayout(context).apply {
    orientation = LinearLayout.HORIZONTAL
    gravity = Gravity.CENTER_VERTICAL
  }
  private val language = TextView(context).apply { setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f) }
  private val buttons = LinearLayout(context).apply { orientation = LinearLayout.HORIZONTAL }
  private val scroll = HorizontalScrollView(context).apply {
    isHorizontalScrollBarEnabled = false
    isFillViewport = false
    overScrollMode = OVER_SCROLL_NEVER
  }
  private val body = TextSegmentView(context)
  private val host = Handler(Looper.getMainLooper())
  private val cardBackground = GradientDrawable().apply { cornerRadius = 6f * resources.displayMetrics.density }
  private val barPaint = Paint(Paint.ANTI_ALIAS_FLAG)

  private var segment: CodeSegment? = null
  private var rendered: RenderedDocument? = null
  private var controlLabels: List<String> = DEFAULT_LABELS
  private var controls = 0
  private var controlsEnabled = false
  private var wrapped = false
  private var copied = false
  private var savedOffset = 0
  private var restoreScroll = false
  private var controlHost: ControlHost? = null

  /**
   * The block's ordinal among the document's code blocks, which a toggle is
   * keyed by. It comes from the segment rather than from a count the view walks,
   * so it is the same number the measurement used.
   */
  var ordinal: Int = -1

  private val copyButton = controlButton(R.drawable.ds_ic_copy, LABEL_COPY_CODE)
  private val downloadButton = controlButton(R.drawable.ds_ic_download, LABEL_DOWNLOAD_FILE)
  private val wrapButton = controlButton(R.drawable.ds_ic_wrap, LABEL_WRAP_CODE)

  private val clearCopied = Runnable {
    copied = false
    updateButtons()
  }

  init {
    setWillNotDraw(false)
    addView(header)
    addView(scroll)
    header.addView(
      language,
      LayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.MATCH_PARENT).apply { gravity = Gravity.START },
    )
    header.addView(buttons, LayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.MATCH_PARENT))
    buttons.addView(wrapButton)
    buttons.addView(downloadButton)
    buttons.addView(copyButton)
    scroll.addView(body, LayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.MATCH_PARENT))
  }

  private fun controlButton(drawable: Int, label: Int): ImageButton = ImageButton(context).apply {
    setImageResource(drawable)
    minimumWidth = TOUCH_SIZE
    minimumHeight = TOUCH_SIZE
    setPadding(0, 0, 0, 0)
    scaleType = ImageView.ScaleType.CENTER
    contentDescription = controlLabels[label]
    setOnClickListener {
      when (it) {
        wrapButton -> controlHost?.onToggleWrap(ordinal, segment?.language.orEmpty(), !wrapped)
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
    // The tail block is rebuilt on every snapshot while its fence is open, so
    // the offset is kept only while the block still looks like the same one.
    val same = this.segment != null && this.segment?.firstLine == segment.firstLine
    savedOffset = if (same) scroll.scrollX else 0
    restoreScroll = true
    ordinal = segment.codeOrdinal
    this.segment = segment
    cardBackground.setColor(segment.colors.codeBackground)
    body.setContent(segment.text, segment.accessibleBlocks, rendered)
    language.text = segment.language
    updateButtons()
    requestLayout()
    invalidate()
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

  fun setWrapped(wrapped: Boolean) {
    if (wrapped == this.wrapped) return
    this.wrapped = wrapped
    updateButtons()
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
    wrapButton.visibility = if (controls and CONTROL_CODE_WRAP != 0) View.VISIBLE else View.GONE
    downloadButton.visibility = if (controls and CONTROL_CODE_DOWNLOAD != 0) View.VISIBLE else View.GONE
    copyButton.visibility = if (controls and CONTROL_CODE_COPY != 0) View.VISIBLE else View.GONE
    for (button in listOf(wrapButton, downloadButton, copyButton)) {
      button.isEnabled = controlsEnabled && button.visibility == View.VISIBLE
      button.alpha = if (button.isEnabled) 1f else 0.4f
    }
    copyButton.setImageResource(if (copied) R.drawable.ds_ic_check else R.drawable.ds_ic_copy)
    copyButton.contentDescription = controlLabels[if (copied) LABEL_COPIED else LABEL_COPY_CODE]
    wrapButton.isSelected = wrapped
    wrapButton.contentDescription = controlLabels[if (wrapped) LABEL_UNWRAP_CODE else LABEL_WRAP_CODE]
    wrapButton.stateDescription = wrapButton.contentDescription
  }

  private fun markCopied() {
    copied = true
    host.removeCallbacks(clearCopied)
    host.postDelayed(clearCopied, COPIED_DURATION_MS)
    ControlDelivery.announceCopied(copyButton, controlLabels[LABEL_COPIED])
    updateButtons()
  }

  override fun onLayout(changed: Boolean, left: Int, top: Int, right: Int, bottom: Int) {
    val segment = this.segment ?: return
    val rendered = this.rendered ?: return
    val headerHeight = if (header.visibility == View.VISIBLE) header.measuredHeight else 0
    val padding = segment.padding
    val leading = segment.contentLeading
    val cardWidth = (right - left - leading).coerceAtLeast(0)
    val bodyWidth = segment.bodyWidth(rendered.paint, cardWidth, wrapped)
    val bodyHeight = segment.bodyHeight(rendered.paint, cardWidth, wrapped)
    header.layout(left, top, right, top + headerHeight)
    val scrollLeft = left + leading + padding
    val scrollRight = (right - padding).coerceAtLeast(scrollLeft)
    scroll.layout(scrollLeft, top + headerHeight + padding, scrollRight, (bottom - padding).coerceAtLeast(top))
    body.measure(
      MeasureSpec.makeMeasureSpec(bodyWidth, MeasureSpec.EXACTLY),
      MeasureSpec.makeMeasureSpec(bodyHeight, MeasureSpec.EXACTLY),
    )
    body.layout(0, 0, bodyWidth, bodyHeight)
    if (restoreScroll) {
      val maximum = (bodyWidth - scroll.width).coerceAtLeast(0)
      scroll.scrollTo(savedOffset.coerceIn(0, maximum), 0)
      restoreScroll = false
    }
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
    cardBackground.setBounds(0, 0, width, height)
    cardBackground.draw(canvas)
    val headerHeight = if (header.visibility == View.VISIBLE) header.measuredHeight else 0
    val firstLine = segment.accessibleBlocks.firstOrNull()
    for (container in segment.containers) {
      val bar = container.barColor
      if (bar != null) {
        barPaint.color = bar
        val x = container.leading + 2f
        val barWidth = 3f * resources.displayMetrics.density
        canvas.drawRect(x, headerHeight.toFloat(), x + barWidth, height.toFloat(), barPaint)
      }
      val marker = container.marker
      if (marker != null && firstLine != null) {
        val markerPaint = TextPaint().apply { textSize = 12f * resources.displayMetrics.scaledDensity }
        val baseline = headerHeight + segment.padding + markerPaint.fontMetrics.ascent
        val x = container.leading + container.width - 6f - markerPaint.measureText(marker)
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
  private val copyButton = ImageButton(context).apply {
    setImageResource(R.drawable.ds_ic_copy)
    minimumWidth = TOUCH_SIZE
    minimumHeight = TOUCH_SIZE
    setPadding(0, 0, 0, 0)
    scaleType = ImageView.ScaleType.CENTER
  }
  private val downloadButton = ImageButton(context).apply {
    setImageResource(R.drawable.ds_ic_download)
    minimumWidth = TOUCH_SIZE
    minimumHeight = TOUCH_SIZE
    setPadding(0, 0, 0, 0)
    scaleType = ImageView.ScaleType.CENTER
  }
  private var segment: TableSegment? = null
  private var sourceEpoch = -1
  private var pendingLeadingDistance: Int? = 0
  private var controlLabels: List<String> = DEFAULT_LABELS
  private var controls = 0
  private var controlsEnabled = false
  private var controlHost: ControlHost? = null

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

  fun setSegment(segment: TableSegment, sourceEpoch: Int) {
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
    grid.updateSegment(segment, resetFocus = !sameTable)
    requestLayout()
    invalidate()
  }

  fun resetScroll() {
    sourceEpoch = -1
    pendingLeadingDistance = 0
    requestLayout()
  }

  override fun onLayout(changed: Boolean, left: Int, top: Int, right: Int, bottom: Int) {
    val segment = segment ?: return
    val toolbarHeight = if (toolbar.visibility == View.VISIBLE) toolbar.measuredHeight else 0
    toolbar.layout(left, top, right, top + toolbarHeight)
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
    val toolbarHeight = if (toolbar.visibility == View.VISIBLE) toolbar.measuredHeight else 0
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
    val width = segment.tableWidth.toFloat() - 1
    if (segment.hasHeader && segment.rowHeights.isNotEmpty()) {
      paint.color = segment.colors.codeBackground
      canvas.drawRect(0f, 0f, width, segment.rowHeights[0].toFloat(), paint)
    }
    segment.cells.forEachIndexed { row, cells ->
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
    canvas.drawRect(0f, 0f, width, 1f, paint)
    for (height in segment.rowHeights) {
      y += height
      canvas.drawRect(0f, y, width, y + 1f, paint)
    }
    var x = if (segment.rtl) width else 0f
    canvas.drawRect(x, 0f, x + 1f, y, paint)
    for (columnWidth in segment.columnWidths) {
      x += if (segment.rtl) -columnWidth else columnWidth
      canvas.drawRect(x, 0f, x + 1f, y, paint)
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
