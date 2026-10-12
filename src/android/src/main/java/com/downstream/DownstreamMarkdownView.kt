package com.downstream

import android.content.Context
import android.content.res.Configuration
import android.graphics.Rect
import android.os.Handler
import android.os.Looper
import android.view.View
import android.view.ViewGroup
import android.view.ViewTreeObserver
import android.view.accessibility.AccessibilityManager
import java.lang.ref.WeakReference
import com.facebook.react.bridge.Arguments
import com.facebook.react.uimanager.PixelUtil
import com.facebook.react.uimanager.StateWrapper

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

/// `RevealEffect`'s values, as the flat timing array's first number.
private const val EFFECT_NONE = 0
private const val EFFECT_FADE = 1
private const val EFFECT_RISE = 2

/// The seven numbers `src/animation.ts` resolves: `[duration, stagger,
/// maxBacklog, x1, y1, x2, y2]`.
private const val JS_TIMING_LENGTH = 7

/// The array the engine reads: `[effect, split]`, the seven numbers above, and
/// the animator scale last.
private const val TIMING_LENGTH = 10

/**
 * Fabric host view for <DownstreamMarkdown>: stacks segment views at the
 * frames the measurement computed. Documents arrive through Fabric state; the
 * view never parses.
 */
class DownstreamMarkdownView(context: Context) : ViewGroup(context), ControlHost {
  /**
   * The layer the segment views live in.
   *
   * React mounts this view's children — one `DownstreamFenceSlot` per hosted
   * fence — directly on the view, in React's own order, which is the order
   * Fabric addressed them in. Segments therefore cannot live there too: the
   * index-reconciling loop below would delete a slot it did not recognize and
   * lose the reader's scroll position every time a renderer mounted. One layer
   * for each keeps the two reconciliations apart, and the segment layer is
   * always index 0 so a stray child cannot be mistaken for a segment.
   */
  private val segmentLayer = SegmentLayer(context)

  /**
   * Where Fabric puts a hosted child, above the segments.
   *
   * Its own `onLayout` does nothing, as `ReactViewGroup`'s does: Fabric has
   * already given each child its frame from the shadow node, which is the frame
   * the document's measurement reserved for it.
   */
  internal val hostLayer = HostLayer(context)

  internal var onLinkPress: ((view: DownstreamMarkdownView, url: String, start: Int, end: Int) -> Unit)? = null
  internal var onControl:
    ((view: DownstreamMarkdownView, block: String, action: String, format: String, language: String,
      filename: String, url: String) -> Unit)? = null
  internal var onControlFailure:
    ((view: DownstreamMarkdownView, block: String, action: String, message: String) -> Unit)? = null
  internal var onWrapToggle: ((view: DownstreamMarkdownView, ordinal: Int, wrapped: Boolean) -> Unit)? = null
  internal var onExpandToggle: ((view: DownstreamMarkdownView, ordinal: Int, expanded: Boolean) -> Unit)? = null

  /**
   * Phase 2. Both fire before the view applies its own choice, so the manager
   * reads `nextToggledOrdinal` while it is still the press's ordinal.
   */
  internal var onTableExpandToggle: ((view: DownstreamMarkdownView, ordinal: Int, expanded: Boolean) -> Unit)? =
    null

  internal var onTableFullscreen: ((view: DownstreamMarkdownView, ordinal: Int) -> Unit)? = null
  internal var onDetailsToggle: ((view: DownstreamMarkdownView, containerId: Int, open: Boolean) -> Unit)? = null
  internal var onTaskToggle: ((view: DownstreamMarkdownView, containerId: Int, checked: Boolean) -> Unit)? = null
  internal var onLinkLongPress: ((view: DownstreamMarkdownView, url: String, start: Int, end: Int) -> Unit)? = null
  internal var onTagPress: ((
    view: DownstreamMarkdownView,
    name: String,
    attributes: List<String>,
    text: String,
    range: IntRange,
  ) -> Unit)? = null

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
  /**
   * The `dir` prop, as a text direction. Null means `auto`, which is the engine's
   * per-block decision rather than one the view makes.
   */
  private var direction = View.TEXT_DIRECTION_FIRST_STRONG_LTR

  /**
   * Per-URL link styles, in four flat arrays as the bridge carries them.
   *
   * Held as parts rather than as one object because the four props arrive as
   * four updates: the object is built where it is read, from whatever parts
   * have arrived, so an update that changes one array does not drop the other
   * three.
   */
  private var variantPrefixes: List<String> = emptyList()
  private var variantCounts: List<Int> = emptyList()
  private var variantColors: List<Int> = emptyList()
  private var variantUnderlines: List<Boolean> = emptyList()

  fun setLinkVariantPrefixes(value: List<String>) {
    variantPrefixes = value
  }

  fun setLinkVariantCounts(value: List<Int>) {
    variantCounts = value
  }

  fun setLinkVariantColors(value: List<Int>) {
    variantColors = value
  }

  fun setLinkVariantUnderline(value: List<Boolean>) {
    variantUnderlines = value
  }

  private var tagStyleNames: List<String> = emptyList()
  private var tagStyleColors: List<Int> = emptyList()
  private var tagStyleFlags: List<Int> = emptyList()

  fun setTagStyleNames(value: List<String>) {
    tagStyleNames = value
  }

  fun setTagStyleColors(value: List<Int>) {
    tagStyleColors = value
  }

  fun setTagStyleFlags(value: List<Int>) {
    tagStyleFlags = value
  }

  /** The four parts as one object, built where it is read. */
  internal fun linkVariants(): LinkVariants =
    LinkVariants(variantPrefixes, variantCounts, variantColors, variantUnderlines)
  private var syntaxHighlighting = true
  private var incrementalParagraphs = false
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

  /**
   * The table layout, beside [codeLayout] and for the same reason: the shadow
   * node measures with it and the view draws with it, so a measured table and a
   * drawn one cannot disagree about a cap.
   */
  private var tableLayout = TableLayout()

  /**
   * The reader's one set of choices, beside the two layouts that carry it.
   *
   * A collapsible block's toggle reads from here rather than from either layout,
   * because it is neither a code block's nor a table's decision, and putting it
   * in one of them would make that kind's state a place the other two are read.
   */
  private var readerChoices = ReaderChoices()
  private var csvSeparator = ","
  private var codeFilename = "file"
  private var tableFilename = "table"
  private var imageFilename = "image"
  private var parseFlags = 0
  private var allowedImagePrefixes: List<String> = emptyList()
  private var exporter = DocumentExporter(context)
  /**
   * Which fenced blocks a React child replaced, and how tall each child is.
   *
   * Carried here rather than read from props because a height belongs to the
   * measurement rather than to a prop, and because the shadow node is what knows
   * a child mounted: a fence with no child is drawn as the code card, and this is
   * how the view finds out.
   */
  private var hostLayout = HostLayout()
  private var generation = 0
  private var sessionId: String? = null
  private var sourceEpoch = 0
  private var pendingTraceDocumentId = 0L

  init {
    // The segment layer is always the first child, so the document's own drawing
    // is under anything Fabric mounts for a slot and a stale child can never be
    // mistaken for a segment.
    addView(segmentLayer, 0, LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT))
    addView(hostLayer, 1, LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT))
  }

  /**
   * The segments and the hosted children, in the order a reader meets them.
   *
   * A slot's own elements come after the paragraph above it and before the one
   * below it, which is the order they are drawn in and the order a screen reader
   * must read them in: the segment layer's virtual nodes are the text, so
   * without the sort below every chart would be announced after the whole
   * message.
   */
  override fun addChildrenForAccessibility(out: ArrayList<View>) {
    if (out == null) {
      throw UnsupportedOperationException("DownstreamMarkdownView does not support null arguments")
    }
    for (index in 0 until segmentLayer.childCount) {
      out.add(segmentLayer.getChildAt(index))
    }
    for (index in 0 until hostLayer.childCount) {
      val child = hostLayer.getChildAt(index)
      // A hidden child is one whose block has gone from this document; it is not
      // drawn, so it is not read.
      if (child.visibility == View.VISIBLE) {
        out.add(child)
      }
    }
    // Ordered by frame, which is the order the shadow node placed them in: a
    // sorted-by-frame list is the order the reader sees, and the order the two
    // layers produce it in is document order for the segments and React order
    // for the slots.
    out.sortBy { it.top }
  }

  // ------------------------------------------------------------- the fade and the caret

  /**
   * The engine's tracker for this view, as a handle into the JNI layer's table.
   *
   * A tracker is per view and nothing else: it holds the document this view last
   * showed, so two views of the same controlled source each fade their own copy.
   * It is created on the first document and disposed when the view is recycled.
   */
  private var revealHandle = 0L

  /** The resolved timeline, as the flat array the native side reads. */
  private var animationTiming: DoubleArray? = null

  /**
   * The three props the array is built from. They arrive as separate setters in
   * any order within one update, so each is kept and the array rebuilt from all
   * three, rather than any one setter assuming the others have landed.
   */
  private var animationEffect = EFFECT_NONE
  private var animationSplitValue = 0
  private var animationValues: DoubleArray? = null

  /**
   * The caret the engine resolved for the document on screen.
   *
   * It is part of the rendering rather than of the props, because its placeholder
   * is measured with the text, so the view only ever draws what the layout already
   * reserved room for.
   */
  private var caret = PlacedCaret.NONE

  /**
   * The units still fading, as `[block, start, end]` triples with a start time
   * each, in the order the engine scheduled them.
   *
   * Held here rather than asked for per frame: the engine's units are the ones
   * every draw pass reads, and asking again per segment per frame would be a
   * crossing per segment.
   */
  private var revealRanges = LongArray(0)
  private var revealStarts = DoubleArray(0)
  private val revealTargets = RevealTargets()
  private var revealSegmentEnds = IntArray(0)

  /**
   * Takes the engine's units and rebuilds the frame the segments draw from.
   *
   * The frame is rebuilt here as well as on every tick, so the draw that follows a
   * new snapshot already has the new units at alpha zero rather than showing
   * them for a frame at full alpha before the first tick.
   */
  private fun setRevealUnits(ranges: LongArray, starts: DoubleArray) {
    // Previously fading roots also need a redraw when their last unit finishes.
    invalidateReveal()
    revealRanges = ranges
    revealStarts = starts
    buildRevealFrame()
    revealTargets.build(ranges, revealSegmentEnds)
    invalidateReveal()
    revealTargets.retainActive(frame, revealSegmentEnds)
  }

  /** The frame at this instant, or an empty one when nothing is fading. */
  private fun buildRevealFrame() {
    val timing = animationTiming ?: return
    val current = frame ?: RevealFrame(timing, resources.displayMetrics.density, revealRises).also { frame = it }
    current.build(revealRanges, revealStarts, uptimeMillis())
  }

  /** The tick currently queued, so a second apply does not queue a second loop. */
  private var revealTickQueued = false
  private val revealTickCallback = Runnable { revealTick() }

  /** The animator scale, which multiplies every duration and can turn the fade off. */
  private var animatorScale = 1f

  /**
   * Whether this view has taken the session's "already shown" record yet.
   *
   * Once, because seeding again after the tracker has shown something would reset
   * the very history the tracker is keeping.
   */
  private var revealSeeded = false

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
    // A new generation, a rebind or a new session has nothing to do with what this
    // view last showed, so the tracker forgets it. The next source animates from
    // its first word rather than resuming this one's timing.
    if (revealHandle != 0L) {
      DownstreamNative.revealClear(revealHandle)
      setRevealUnits(LongArray(0), DoubleArray(0))
      revealSeeded = false
      stopRevealTick()
    }
    // A new generation has nothing left to pin and no offsets worth keeping, so
    // every reader scroll position goes, tables' and code cards' alike.
    for (child in segmentViews) {
      when (child) {
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
    this.documentId = documentId
    this.fontScale = fontScale
    this.themeId = resolved
    applyDocumentState(refreshRendering = true, updateReveal = true)
  }

  /**
   * Applies one Fabric state before rendering or reconciling any segment.
   * Separate setters would rebuild the previous document with the new caret
   * and restack it with intermediate image and host layouts.
   */
  internal fun applyState(
    wrapper: StateWrapper,
    documentId: Long,
    fontScale: Float,
    stateThemeId: Long,
    code: CodeLayout,
    images: ImageLayout,
    hosts: HostLayout,
    chrome: Int,
    caret: PlacedCaret,
    tables: TableLayoutProps,
    incrementalParagraphs: Boolean = false,
  ) {
    stateWrapper = wrapper
    val resolved = if (stateThemeId != 0L) stateThemeId else internProps()
    val documentChanged = documentId != this.documentId || fontScale != this.fontScale ||
      resolved != themeId || incrementalParagraphs != this.incrementalParagraphs ||
      (documentId != 0L && rendered == null)
    val refreshRendering = documentChanged || caret != this.caret
    val codeChanged = code != codeLayout
    val hostsChanged = hosts != hostLayout
    val tablesChanged = tables != tableLayout.props
    val choicesChanged = code.choices != readerChoices
    val layoutChanged = codeChanged || hostsChanged || images != imageLayout || chrome != this.chrome ||
      tablesChanged || choicesChanged
    if (!refreshRendering && !layoutChanged) return

    this.documentId = documentId
    this.fontScale = fontScale
    themeId = resolved
    codeLayout = code
    imageLayout = images
    hostLayout = hosts
    this.chrome = chrome
    this.caret = caret
    this.incrementalParagraphs = incrementalParagraphs
    // One set of reader choices and one table layout, derived from the same
    // state the shadow node measured with: a code toggle, a table toggle, and
    // a details toggle all live in it.
    readerChoices = code.choices
    tableLayout = TableLayout(tables, code.choices)
    applyDocumentState(refreshRendering, updateReveal = documentChanged)
    if (codeChanged || tablesChanged || choicesChanged) requestLayout()
    if (hostsChanged) layoutHostChildren()
  }

  /** Reconciles only after every field belongs to the incoming state. */
  private fun applyDocumentState(refreshRendering: Boolean, updateReveal: Boolean) {
    if (pendingTraceDocumentId != 0L && pendingTraceDocumentId != documentId) {
      DownstreamNative.traceDocument(pendingTraceDocumentId, "frame_unavailable")
      pendingTraceDocumentId = 0L
    }
    val tracing = updateReveal && documentId != 0L && DownstreamNative.isTracingDocument(documentId)
    if (tracing) DownstreamNative.traceDocument(documentId, "apply_start")
    if (refreshRendering) {
      val previous = rendered?.documentId ?: 0L
      rendered = if (documentId != 0L) {
        RenderedDocument.obtain(context, documentId, fontScale, syntaxHighlighting, themeId, caret, linkVariants(), incrementalParagraphs = incrementalParagraphs && direction != View.TEXT_DIRECTION_RTL)
      } else {
        null
      }
      // This view moved on, so the snapshot it showed is not mounted again.
      if (previous != 0L && previous != documentId) RenderedDocument.retire(previous)
    }
    syncSegmentViews()
    // The fade is decided after the rendering exists, because the tracker aligns
    // the document it was given against the one this view last showed, and the
    // segment views have to be there for the tick to invalidate.
    if (updateReveal) applyReveal()
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
    observeSegmentViewport()
    scheduleTraceFrame()
    buildRevealFrame()
    invalidateReveal()
    startRevealTick()
  }

  override fun onDetachedFromWindow() {
    stopRevealTick()
    segmentViewportObserver?.let { if (it.isAlive) it.removeOnPreDrawListener(segmentViewportListener) }
    segmentViewportObserver = null
    segmentViewportKnown = false
    super.onDetachedFromWindow()
  }

  /**
   * The `dir` prop, as a text direction the view draws with.
   *
   * `auto` leaves each text view on the engine's per-block answer, which is what
   * the measurement already used; the two other values pin every block the same
   * way. Null is `auto`, which is the prop's own default.
   */
  fun setDirection(value: String?) {
    val next = when (value) {
      "ltr" -> View.TEXT_DIRECTION_LTR
      "rtl" -> View.TEXT_DIRECTION_RTL
      else -> View.TEXT_DIRECTION_FIRST_STRONG_LTR
    }
    if (next == direction) return
    direction = next
    applyDocumentState(refreshRendering = true, updateReveal = false)
    requestLayout()
  }

  fun setIncrementalParagraphs(enabled: Boolean) {
    if (enabled == incrementalParagraphs) return
    incrementalParagraphs = enabled
    applyDocumentState(refreshRendering = true, updateReveal = false)
    requestLayout()
  }

  fun setSelectable(selectable: Boolean) {
    if (this.selectable == selectable) return
    this.selectable = selectable
    syncSegmentViews()
    applySettledState()
  }

  fun setControls(controls: Int) {
    if (controls == this.controls) return
    this.controls = controls
    applySettledState()
  }

  fun setControlLabels(value: List<String>) {
    controlLabels = if (value.size >= DEFAULT_LABELS.size) value else DEFAULT_LABELS
    for (child in segmentViews) {
      when (child) {
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
   * The parse flags, as the engine's bitmask.
   *
   * Held so an unresolved formula can be told apart from one that was never meant to
   * be laid out: with the math flag off the engine leaves the literal alone and this
   * view has nothing to do, and with it on an unresolved formula draws in the theme's
   * error color. The inline HTML and script-syntax bits are the parser's business and
   * are already in the state. Both answers are otherwise in the state.
   */
  fun setParseFlags(value: Int) {
    parseFlags = value
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

  /**
   * The reader's own per-block choices, as the state carries them.
   *
   * Only the two code sets are replaced. A state update carries all four, so the
   * other two arrive again on the next one, and reading them from the current
   * value rather than from `{}` is what keeps a table toggle from being undone by
   * a code state update that happened to land later.
   */
  fun setCodeChoices(wrapToggles: IntArray, expanded: IntArray) {
    val choices = codeLayout.choices.replace(ChoiceKind.Wrap, wrapToggles)
      .replace(ChoiceKind.CodeExpand, expanded)
    if (choices == codeLayout.choices) return
    codeLayout = CodeLayout(codeLayout.props, choices)
    applySettledState()
    requestLayout()
  }

  /**
   * The table cap, from `tableMaxHeight`. Separate from [setCodeProps] because the
   * two caps are separate numbers with separate defaults, and one method taking
   * both would have to be told which a caller meant.
   */
  fun setTableProps(maxHeightDp: Double) {
    val props = TableLayoutProps(PixelUtil.toPixelFromDIP(maxHeightDp).toFloat())
    if (props == tableLayout.props) return
    tableLayout = TableLayout(props, tableLayout.choices)
    applySettledState()
    requestLayout()
  }

  /** The reader's own per-table choices, as the state carries them. */
  fun setTableChoices(expanded: IntArray) {
    val choices = tableLayout.choices.replace(ChoiceKind.TableExpand, expanded)
    if (choices == tableLayout.choices) return
    tableLayout = tableLayout.withChoices(choices)
    applySettledState()
    requestLayout()
  }

  /** The collapsible blocks the reader opened, as the state carries them. */
  fun setDetailsChoices(open: IntArray) {
    val choices = readerChoices.replace(ChoiceKind.DetailsOpen, open)
    if (choices == readerChoices) return
    readerChoices = choices
    applySettledState()
    requestLayout()
  }

  fun setChrome(value: Int) {
    if (value == chrome) return
    chrome = value
    applySettledState()
  }

  /** `animation`: `none`, `fadeIn` or `slideUp`, already resolved in JavaScript. */
  internal fun setAnimationEffect(value: String?) {
    animationEffect = when (value) {
      "fadeIn" -> EFFECT_FADE
      "slideUp" -> EFFECT_RISE
      else -> EFFECT_NONE
    }
    updateAnimationTiming()
  }

  /** `animationSplit`: `word` or `char`. */
  internal fun setAnimationSplit(value: String?) {
    animationSplitValue = if (value == "char") 1 else 0
    updateAnimationTiming()
  }

  /** `animationTiming`: the seven numbers, or null when the fade is off. */
  internal fun setAnimationValues(values: DoubleArray?) {
    animationValues = values
    updateAnimationTiming()
  }

  /**
   * The fade's options, as the flat array the native side reads.
   *
   * Null turns the fade off, which is the default and costs nothing: no tracker
   * is created, no tick is posted, and every draw pass answers an empty set.
   */
  private fun updateAnimationTiming() {
    val values = animationValues
    val timing = if (animationEffect == EFFECT_NONE || values == null || values.size < JS_TIMING_LENGTH) {
      null
    } else {
      animatorScale = readAnimatorScale()
      // The scale rides at the end of the same flat array rather than as a prop of
      // its own: the engine applies it, so there is one place that multiplies a
      // duration rather than two that each think they do.
      DoubleArray(TIMING_LENGTH).also {
        it[0] = animationEffect.toDouble()
        it[1] = animationSplitValue.toDouble()
        values.copyInto(it, 2, 0, JS_TIMING_LENGTH)
        it[TIMING_LENGTH - 1] = animatorScale.toDouble()
      }
    }
    if (timing == null) {
      if (animationTiming == null) return
      animationTiming = null
      frame = null
      revealRises = false
      // An explicit "off" finishes what is running rather than letting it
      // continue: this was a request, so the text snaps.
      finishReveal()
      return
    }
    if (animationTiming?.contentEquals(timing) == true) return
    animationTiming = timing
    // The effect is the first number, and it is the only thing that changes what a
    // draw pass does: a rise translates, a fade does not.
    revealRises = animationEffect == EFFECT_RISE
    // A new curve invalidates the cached interpolator, so the frame is rebuilt
    // rather than reused with the old one.
    frame = null
    if (animatorScale <= 0f) {
      // A reader who has turned animations off gets the text at once and a static
      // caret, which is what every other animation in the platform does.
      finishReveal()
    } else if (documentId != 0L) {
      applyReveal()
    }
  }

  /**
   * The caret the engine resolved for this document, already in the state's data.
   *
   * A change here rebuilds the rendering rather than redrawing: the placeholder
   * moves, so the measured text is a different one.
   */
  internal fun setCaret(value: PlacedCaret) {
    if (value == caret) return
    caret = value
    val id = documentId
    val scale = fontScale
    rendered = if (id != 0L) {
      RenderedDocument.obtain(context, id, scale, syntaxHighlighting, themeId, value, linkVariants = linkVariants(), incrementalParagraphs = incrementalParagraphs && direction != View.TEXT_DIRECTION_RTL)
    } else {
      null
    }
    syncSegmentViews()
  }

  /**
   * Turns on a segment's controls and its selection once every block it covers
   * is settled. A streaming tail is rebuilt on every snapshot and its selection
   * cleared, so a selection there would vanish mid-drag; earlier text is settled
   * and keeps its selection.
   */
  private fun applySettledState() {
    val plan = rendered?.plan ?: return
    val indices: Iterable<Int> = viewportOnlyIndices?.asIterable() ?: (0 until minOf(segmentViews.size, plan.size))
    for (index in indices) {
      val planned = plan[index]
      val settled = planned.from + 1 <= planned.settledBlocks
      when (val child = segmentViews[index]) {
        is TextSegmentView -> {
          // A hosted tag replaces its whole segment: the child owns the range
          // now, so the text underneath draws nothing. Only a segment the plan
          // grouped for one container is ever hosted, which is what makes the
          // container id the key rather than a block range.
          val custom = (rendered?.segments?.getOrNull(index) as? TextSegment)?.customContainer ?: -1
          child.hosted = custom >= 0 && hostLayout.hostsTag(custom)
          child.setSelectable(selectable && settled)
          child.textDirection = direction
          // The chevron is the control's own glyph, so it appears with the control
          // and points the way the body will move.
          child.readerChoices = readerChoices
          child.setDetailsState(
            controls and CONTROL_DETAILS_TOGGLE != 0,
            child.isSummaryOpen(readerChoices),
          )
        }
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
          // The same two calls the measurement used, so the box this table is
          // given and the cap it draws inside it are one decision.
          child.setControls(controls)
          child.setControlsEnabled(settled)
          child.setToolbarVisible(chrome and RenderedDocument.CHROME_TABLE_TOOLBAR != 0)
          child.tableLayout = resolveTable(tableLayout, child.ordinal)
          // Settled on the same rule a code card uses: the plan carries the
          // parser's own settled count, so the card and the parser cannot
          // disagree about whether the table closed.
          child.isStreaming = !settled
        }
        is ImageSegmentView -> child.setDownloadControl(controls, settled, controlLabels[LABEL_IMAGE_DOWNLOAD])
      }
    }
  }

  // ------------------------------------------------------------- the fade

  /**
   * Aligns the document on screen against the one this view last showed, takes
   * the units that are still fading, and starts the tick if any are left.
   *
   * The engine holds every decision: which characters are new, where the unit
   * boundaries fall, and when each unit starts. Nothing here parses, aligns or
   * times anything of its own.
   */
  private fun applyReveal() {
    val timing = animationTiming
    val id = documentId
    if (timing == null || id == 0L || animatorScale <= 0f) {
      stopRevealTick()
      if (id != 0L && timing == null) {
        // With the fade off the tracker still records what was shown, so turning
        // it on later does not reveal a document that has been on screen all
        // along.
        DownstreamNative.revealNoteShown(revealHandle(), id, sessionId)
      }
      return
    }
    val handle = revealHandle()
    // A view bound to a live stream starts from what the session has already put
    // on screen, so a message scrolled back into view does not fade all of itself
    // again. A controlled view has no session and fades its first document.
    if (handle != 0L && !revealSeeded) {
      revealSeeded = true
      DownstreamNative.revealSeed(handle, sessionId)
    }
    val units = DownstreamNative.revealApply(handle, id, uptimeMillis(), timing)
    if (units == null) {
      setRevealUnits(LongArray(0), DoubleArray(0))
      stopRevealTick()
      return
    }
    setRevealUnits(units[0] as? LongArray ?: LongArray(0), units[1] as? DoubleArray ?: DoubleArray(0))
    startRevealTick()
  }

  /** The tracker's handle, created on first use so an unanimated view has none. */
  private fun revealHandle(): Long {
    if (revealHandle == 0L) {
      revealHandle = DownstreamNative.revealCreate()
    }
    return revealHandle
  }

  /**
   * The tick, one per view.
   *
   * The same runnable is posted and canceled, so a stop followed by a restart
   * cannot leave an older callback driving a second animation loop.
   */
  private fun startRevealTick() {
    if (revealTickQueued || revealRanges.isEmpty() || !isAttachedToWindow) return
    revealTickQueued = true
    postOnAnimation(revealTickCallback)
  }

  private fun stopRevealTick() {
    removeCallbacks(revealTickCallback)
    revealTickQueued = false
  }

  /**
   * One frame of the fade.
   *
   * The cost is a prune on the native side and an invalidate per affected
   * segment: no parse, no relayout, and no change to any cached text, which is
   * what a frame of animation has to avoid.
   */
  private fun revealTick() {
    revealTickQueued = false
    val timing = animationTiming
    val handle = revealHandle
    if (timing == null || handle == 0L || !isAttachedToWindow) {
      stopRevealTick()
      return
    }
    // The alphas move with the clock, so the frame is rebuilt before the redraw
    // that reads it.
    buildRevealFrame()
    invalidateReveal()
    // A finished root has now been dirtied for its final opaque display list.
    // Retire it before the next tick while other roots can keep fading.
    revealTargets.retainActive(frame, revealSegmentEnds)
    if (DownstreamNative.revealPrune(handle, uptimeMillis(), timing)) {
      // The prune dropped what finished, so this frame draws it at full alpha and
      // the next has nothing left to do.
      startRevealTick()
    } else {
      setRevealUnits(LongArray(0), DoubleArray(0))
    }
  }

  /**
   * Finishes every fade at once and redraws.
   *
   * A scale of zero or an explicit `animated: false` shows all text at once.
   * Detaching only pauses callbacks and retains the timeline for reattachment.
   */
  private fun finishReveal() {
    stopRevealTick()
    val handle = revealHandle
    if (handle == 0L) return
    DownstreamNative.revealClear(handle)
    setRevealUnits(LongArray(0), DoubleArray(0))
    revealSeeded = false
    invalidateReveal()
  }

  /**
   * The animator duration scale, which multiplies every duration in the timeline
   * and turns the fade off at zero.
   *
   * Read on the UI thread once per apply rather than observed: it changes only
   * from Settings, and a fade already running is finished when it does.
   */
  private fun readAnimatorScale(): Float = DownstreamNative.animatorDurationScale(context)

  private fun uptimeMillis(): Double = android.os.SystemClock.uptimeMillis().toDouble()

  // ------------------------------------------------------ the draw side of the fade

  /**
   * The frame every segment under this view draws from, or null when nothing is
   * fading.
   *
   * Built once per tick from the engine's flat arrays and then read by each
   * segment's draw pass, so a frame costs one parse rather than one per segment.
   */
  private var frame: RevealFrame? = null

  /** The fade's effect, so a `slideUp` knows to translate and a `fadeIn` does not. */
  private var revealRises = false

  /** The units fading right now, or null when the fade is off or finished. */
  internal fun revealFrame(): RevealFrame? = frame

  /**
   * The alpha at the first character of a document block, 1 for anything that is
   * not fading.
   *
   * For the views that draw beside a text layout rather than inside one: a table
   * row's fill, a code card's chrome, a list marker. Each of those is one unit of
   * its block, so the block's first unit's alpha is the whole answer.
   */
  internal fun blockAlpha(block: Int): Float {
    val current = frame ?: return 1f
    var best = 1f
    for (index in current.firstUnitForBlock(block) until current.size) {
      val unit = current.unitAt(index)
      if (unit.block != block) break
      if (unit.start > 0) continue
      if (unit.alpha < best) best = unit.alpha
    }
    return best
  }

  /** Redraws whatever the fade touched. */
  internal fun invalidateReveal() {
    // Targets retain the previous frame until this invalidation, including
    // roots whose last unit became opaque this tick. Settled siblings keep their
    // display lists. A target is a segment index, so it is looked up among the segments: this
    // view's own children are the segment and host layers.
    for (index in 0 until revealTargets.size) {
      val child = segmentViews.getOrNull(revealTargets.indexAt(index)) ?: continue
      invalidateTree(child)
    }
  }

  /**
   * Invalidates a segment and everything under it.
   *
   * A parent's `invalidate` does not redraw its children's display lists, and a
   * code card's body and gutter and a table's grid are children that read the
   * alpha themselves, so each one is asked.
   */
  private fun invalidateTree(view: View) {
    view.invalidate()
    if (view is ViewGroup) {
      for (index in 0 until view.childCount) {
        invalidateTree(view.getChildAt(index))
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
        rendered = RenderedDocument.obtain(context, documentId, fontScale, syntaxHighlighting, themeId, linkVariants = linkVariants(), incrementalParagraphs = incrementalParagraphs && direction != View.TEXT_DIRECTION_RTL)
        syncSegmentViews()
      }
    }
  }

  fun reset() {
    disposeReveal()
    animationEffect = EFFECT_NONE
    animationSplitValue = 0
    animationValues = null
    animationTiming = null
    animatorScale = 1f
    revealRises = false
    caret = PlacedCaret.NONE
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
    incrementalParagraphs = false
    generation = 0
    sessionId = null
    sourceEpoch++
    codeLayout = CodeLayout()
    tableLayout = TableLayout()
    onControl = null
    onControlFailure = null
    onWrapToggle = null
    onExpandToggle = null
    onTableExpandToggle = null
    onTableFullscreen = null
    onDetailsToggle = null
    onTaskToggle = null
    // A recycled view has no emitter any more. Already opened saves finish,
    // while their callbacks drop events belonging to the previous source.
    exporter.release()
    exporter = DocumentExporter(context)
    removeAllViews()
  }

  /** Invalidates pending exports and picker results when the view goes away. */
  fun release() {
    disposeReveal()
    sourceEpoch++
    exporter.release()
  }

  private fun disposeReveal() {
    stopRevealTick()
    if (revealHandle != 0L) DownstreamNative.revealDispose(revealHandle)
    revealHandle = 0L
    revealRanges = LongArray(0)
    revealStarts = DoubleArray(0)
    revealSeeded = false
    frame = null
    revealTargets.build(revealRanges, revealSegmentEnds)
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

  override fun onToggleTableExpand(ordinal: Int, expanded: Boolean) {
    nextOrdinal = ordinal
    onTableExpandToggle?.invoke(this, ordinal, expanded)
  }

  fun tableMarkdown(ordinal: Int): String? {
    val planned = rendered?.plan?.firstOrNull { it.isTable && it.tableOrdinal == ordinal } ?: return null
    return DownstreamNative.export(documentId, planned.from, planned.to, EXPORT_MARKDOWN, ',')
  }

  override fun onTableFullscreen(ordinal: Int) {
    nextOrdinal = ordinal
    onTableFullscreen?.invoke(this, ordinal)
  }

  override fun onToggleDetails(containerId: Int, open: Boolean) {
    nextOrdinal = containerId
    onDetailsToggle?.invoke(this, containerId, open)
  }

  override fun onToggleTask(containerId: Int, checked: Boolean) {
    nextOrdinal = containerId
    onTaskToggle?.invoke(this, containerId, checked)
  }

  /**
   * The reader's own answer for one collapsible block, through the same Fabric
   * state path a code block's wrap takes.
   *
   * Only Yoga and the chat list around the message can know the message's new
   * height, and nothing else has to be involved: the choice is state, so the node
   * is cloned, measured again and remounted with no React render and no re-parse.
   */
  fun toggleDetails(containerId: Int) {
    if (stateWrapper == null) return
    val choices = readerChoices.copyWith(ChoiceKind.DetailsOpen, containerId)
    readerChoices = choices
    applySettledState()
    requestLayout()
    pushTableState(tableLayout.withChoices(choices))
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
  fun toggleWrap(ordinal: Int) = updateChoice { it.copyWith(ChoiceKind.Wrap, ordinal) }

  /** The same, for the cap the expand button lifts. */
  fun toggleExpand(ordinal: Int) = updateChoice { it.copyWith(ChoiceKind.CodeExpand, ordinal) }

  /**
   * The same, for the cap a table's expand button lifts. Separate from
   * [toggleExpand] because the two are keyed by different ordinals: a code
   * ordinal counts code blocks and a table ordinal counts tables, so one set of
   * choices cannot hold both without a caller being told which a press meant.
   */
  fun toggleTableExpand(ordinal: Int) {
    if (stateWrapper == null) return
    val layout = tableLayout.withChoices(
      tableLayout.choices.copyWith(ChoiceKind.TableExpand, ordinal),
    )
    // The chrome is recomputed from the code layout, which did not change: a
    // table's cap and a code card's wrap are separate bits and this press moved
    // neither, so there is nothing to recompute here.
    tableLayout = layout
    applySettledState()
    requestLayout()
    pushTableState(layout)
  }

  private fun updateChoice(toggle: (ReaderChoices) -> ReaderChoices) {
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
    map.putArray("expanded", intArrayOf(choices.codeExpanded))
    wrapper.updateState(map)
  }

  /**
   * The same, for the table layout. Only the table keys change, so the code keys
   * are sent unchanged rather than dropped: the shadow node keeps one code
   * layout, and a push that omitted them would reset the reader's code choices.
   */
  private fun pushTableState(layout: TableLayout) {
    val wrapper = stateWrapper ?: return
    val map = Arguments.createMap()
    map.putDouble("documentId", documentId.toDouble())
    map.putDouble("fontScale", fontScale.toDouble())
    map.putDouble("chrome", chrome.toDouble())
    map.putDouble("codeBlockWrap", if (codeLayout.props.wrap) 1.0 else 0.0)
    map.putDouble("lineNumbers", if (codeLayout.props.lineNumbers) 1.0 else 0.0)
    map.putDouble("codeBlockMaxHeight", PixelUtil.toDIPFromPixel(codeLayout.props.maxHeight).toDouble())
    map.putArray("wrapToggles", intArrayOf(codeLayout.choices.wrapToggles))
    map.putArray("expanded", intArrayOf(codeLayout.choices.codeExpanded))
    // The cap crosses as points, for the same reason the code cap does.
    map.putDouble("tableMaxHeight", PixelUtil.toDIPFromPixel(layout.props.maxHeight).toDouble())
    map.putArray("tableExpanded", intArrayOf(layout.choices.tableExpanded))
    map.putArray("detailsOpen", intArrayOf(layout.choices.detailsOpen))
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
    if (controls and (CONTROL_TABLE_COPY or CONTROL_TABLE_DOWNLOAD or CONTROL_TABLE_EXPAND or
      CONTROL_TABLE_FULLSCREEN) != 0
    ) {
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
      RenderedDocument.obtain(context, documentId, fontScale, syntaxHighlighting, themeId, linkVariants = linkVariants(), incrementalParagraphs = incrementalParagraphs && direction != View.TEXT_DIRECTION_RTL)?.let {
        rendered = it
        syncSegmentViews()
      }
    }
  }

  internal fun pressLink(index: Int) {
    val link = DownstreamNative.readLink(documentId, index) ?: return
    onLinkPress?.invoke(this, link.url, link.start, link.end)
  }

  /**
   * Blocks as the Markdown that produced them, for "Copy as Markdown".
   *
   * The exporter reads block indices, so the caller maps its selection onto
   * the document first: this only exports, because exporting is the one thing
   * the engine does identically on both platforms.
   */
  internal fun exportMarkdown(from: Int, to: Int): String? {
    if (documentId == 0L || from < 0 || to <= from) return null
    return DownstreamNative.export(documentId, from, to, EXPORT_MARKDOWN, ',')
  }

  /**
   * A hold on a link, reported rather than acted on.
   *
   * A tap opens (or asks, through link safety); a hold is the app's: copy the
   * URL, show a sheet. The library does nothing with it, because doing anything
   * would be a second behavior for one gesture.
   */
  internal fun longPressLink(index: Int) {
    val link = DownstreamNative.readLink(documentId, index) ?: return
    onLinkLongPress?.invoke(this, link.url, link.start, link.end)
  }

  /**
   * A press on an app-allowed tag's run.
   *
   * Reported with the tag's name, its attributes, the pressed text, and the
   * source range — everything `onTagPress` promises, read here rather than in
   * JavaScript because the document is what knows them.
   */
  internal fun pressTag(index: Int, text: String) {
    val tag = DownstreamNative.readTag(documentId, index) ?: return
    onTagPress?.invoke(this, tag.name, tag.attributes, text, tag.range)
  }

  // The segment views this view is currently holding, in document order. Every
  // reconciliation below walks this list rather than the view group's children,
  // because the group also holds the React children mounted over the slots.
  private var segmentViews = mutableListOf<View>()

  // Plain unselectable text can release its TextView while outside the actual
  // ancestor clip. Index placeholders preserve reveal and host reconciliation.
  private class DeferredTextView(context: Context) : View(context)
  private val recycledTextViews = java.util.ArrayDeque<TextSegmentView>()
  private val segmentEligibility = java.util.WeakHashMap<TextSegment, Boolean>()
  private val segmentViewport = Rect()
  private var segmentViewportKnown = false
  private var segmentAccessibilityEnabled = false
  private var segmentViewportObserver: ViewTreeObserver? = null
  private var viewportOnlyIndices: IntArray? = null
  private var virtualizedDocument: RenderedDocument? = null
  private var virtualizedFrames: IntArray? = null
  private var virtualizedInterval = 0 until 0
  private var checkedFrameOrder: IntArray? = null
  private var virtualizedFramesOrdered = false
  private val segmentViewportListener = ViewTreeObserver.OnPreDrawListener {
    if (!selectable) refreshSegmentViewport()
    true
  }

  private fun observeSegmentViewport() {
    val observer = viewTreeObserver
    if (segmentViewportObserver === observer) return
    segmentViewportObserver?.let { if (it.isAlive) it.removeOnPreDrawListener(segmentViewportListener) }
    segmentViewportObserver = observer
    observer.addOnPreDrawListener(segmentViewportListener)
    refreshSegmentViewport()
  }

  private fun refreshSegmentViewport() {
    val visible = Rect()
    val known = isAttachedToWindow && width > 0 && height > 0
    if (known && !getLocalVisibleRect(visible)) visible.setEmpty()
    val accessible = (context.getSystemService(Context.ACCESSIBILITY_SERVICE) as? AccessibilityManager)?.isEnabled == true
    if (known == segmentViewportKnown && visible == segmentViewport && accessible == segmentAccessibilityEnabled) return
    segmentViewportKnown = known
    segmentViewport.set(visible)
    segmentAccessibilityEnabled = accessible
    val document = rendered
    val column = width - paddingLeft - paddingRight
    val frames = if (!selectable && !accessible && known && column > 0) {
      document?.frames(column, chrome, codeLayout, imageLayout, hostLayout, tableLayout, readerChoices)
    } else null
    if (document === virtualizedDocument && frames != null && frames === virtualizedFrames && virtualizedFramesOrdered) {
      val next = viewportSegmentInterval(frames)
      viewportOnlyIndices = (virtualizedInterval.asSequence() + next.asSequence()).distinct().sorted().toList().toIntArray()
    }
    try { syncSegmentViews() } finally { viewportOnlyIndices = null }
  }

  private fun viewportSegmentInterval(frames: IntArray): IntRange {
    if (segmentViewport.isEmpty) return 0 until 0
    val overscan = maxOf((64 * resources.displayMetrics.density).toInt(), segmentViewport.height() / 2)
    val top = segmentViewport.top - segmentLayer.top - paddingTop - overscan
    val bottom = segmentViewport.bottom - segmentLayer.top - paddingTop + overscan
    val count = frames.size / 2
    var low = 0; var high = count
    while (low < high) {
      val middle = (low + high) ushr 1
      if (frames[middle * 2] + frames[middle * 2 + 1] < top) low = middle + 1 else high = middle
    }
    val first = low
    high = count
    while (low < high) {
      val middle = (low + high) ushr 1
      if (frames[middle * 2] <= bottom) low = middle + 1 else high = middle
    }
    return first until low
  }

  private fun checkViewportFrameOrder(frames: IntArray): Boolean {
    if (checkedFrameOrder === frames) return virtualizedFramesOrdered
    checkedFrameOrder = frames
    virtualizedFramesOrdered = (1 until frames.size / 2).all { index ->
      frames[index * 2] >= frames[(index - 1) * 2] &&
        frames[index * 2] + frames[index * 2 + 1] >= frames[(index - 1) * 2] + frames[(index - 1) * 2 + 1]
    }
    return virtualizedFramesOrdered
  }

  private fun canDeferSegment(segment: TextSegment): Boolean = segmentEligibility.getOrPut(segment) {
    segment.customContainer < 0 && segment.caretOffset < 0 && segment.detailsContainers.isEmpty() &&
      segment.detailsBlocks.isEmpty() && segment.taskBlocks.isEmpty() &&
      segment.text.getSpans(0, segment.text.length, LinkSpan::class.java).isEmpty() &&
      segment.text.getSpans(0, segment.text.length, TagSpan::class.java).isEmpty() &&
      segment.text.getSpans(0, segment.text.length, SpoilerSpan::class.java).isEmpty() &&
      segment.text.getSpans(0, segment.text.length, MathInlineSpan::class.java).isEmpty()
  }

  private fun recycleTextView(view: View) {
    segmentLayer.removeView(view)
    if (view is TextSegmentView && recycledTextViews.size < 8) {
      view.clearContent()
      recycledTextViews.addLast(view)
    }
  }

  // Reuse views of the same kind in place, so unchanged segments keep their
  // selection and scroll position across updates.
  private fun syncSegmentViews() {
    val document = rendered
    val segments = document?.segments.orEmpty()
    val column = width - paddingLeft - paddingRight
    val frames = if (!selectable && !segmentAccessibilityEnabled && segmentViewportKnown && column > 0) {
      document?.frames(column, chrome, codeLayout, imageLayout, hostLayout, tableLayout, readerChoices)
    } else null
    val indices: Iterable<Int> = viewportOnlyIndices?.asIterable() ?: segments.indices
    if (viewportOnlyIndices == null) revealSegmentEnds = IntArray(document?.plan?.size ?: 0) { document!!.plan[it].to }
    val overscan = maxOf((64 * resources.displayMetrics.density).toInt(), segmentViewport.height() / 2)
    fun deferAt(index: Int): Boolean {
      val segment = segments[index]
      return segment is TextSegment && frames != null && canDeferSegment(segment) &&
        (segmentViewport.isEmpty || segmentLayer.top + paddingTop + frames[index * 2] > segmentViewport.bottom + overscan ||
          segmentLayer.top + paddingTop + frames[index * 2] + frames[index * 2 + 1] < segmentViewport.top - overscan)
    }
    if (frames != null) while (segmentViews.size < segments.size) segmentViews.add(DeferredTextView(context))
    // Retire first so scrolling upwards can reuse the views below the viewport.
    for (index in indices) {
      if (deferAt(index) && segmentViews.getOrNull(index) !is DeferredTextView) {
        segmentViews.getOrNull(index)?.let { recycleTextView(it) }
        val placeholder = DeferredTextView(context)
        if (index < segmentViews.size) segmentViews[index] = placeholder else segmentViews.add(placeholder)
      }
    }
    for (index in indices) {
      val segment = segments[index]
      var view = segmentViews.getOrNull(index)
      if (deferAt(index)) continue
      val reusable = when (segment) {
        is TextSegment -> view is TextSegmentView
        is CodeSegment -> view is CodeSegmentView
        is TableSegment -> view is TableSegmentView
        is ImageSegment -> view is ImageSegmentView
        is MathSegment -> view is MathSegmentView
      }
      if (!reusable) {
        if (view != null) {
          recycleTextView(view)
          segmentViews.removeAt(index)
        }
        view = when (segment) {
          is TextSegment -> if (recycledTextViews.isEmpty()) TextSegmentView(context) else recycledTextViews.removeLast()
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
        segmentLayer.addView(view)
        segmentViews.add(index, view)
      }
      when (segment) {
        is TextSegment -> (view as TextSegmentView).setSegment(segment, document!!, document!!.plan[index].from)
        is CodeSegment -> (view as CodeSegmentView).apply {
          // Whether this block is a host segment comes from the state, not from
          // the segment: the segment is the same card either way, and only the
          // view knows whether a React child mounted for it.
          setHosted(hostLayout.hosts(segment.codeOrdinal))
          setSegment(segment, document!!, document!!.plan[index].from)
        }
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
    while (segmentViews.size > segments.size) {
      val extra = segmentViews.removeAt(segmentViews.size - 1)
      recycleTextView(extra)
    }
    if (viewportOnlyIndices == null) revealTargets.build(revealRanges, revealSegmentEnds)
    applySettledState()
    layoutSegments()
    virtualizedDocument = if (frames != null) document else null
    virtualizedFrames = frames
    virtualizedInterval = if (frames != null && checkViewportFrameOrder(frames)) viewportSegmentInterval(frames) else 0 until 0
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
   * The hosted children, from the state the shadow node wrote.
   *
   * A change here is a change to which fences draw as cards, so the segments are
   * restacked: a hosted block is as tall as its component, and an unhosted one
   * is as tall as its card.
   */
  internal fun setHostLayout(value: HostLayout) {
    if (hostLayout == value) {
      return
    }
    hostLayout = value
    syncSegmentViews()
    layoutHostChildren()
  }

  /**
   * Lets the host layer lay out after a document change.
   *
   * A host child's frame comes from Fabric, from the rectangle the shadow node's
   * measurement reserved for its block, so there is nothing to position here: a
   * child whose block has gone from the new document is hidden and one that is
   * back is shown by the same state update that reached the shadow node. All this
   * does is ask the layer for a pass, so the change is not a frame late.
   */
  internal fun layoutHostChildren() {
    if (hostLayer.childCount > 0) {
      hostLayer.requestLayout()
    }
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
    val frames = document.frames(
      width, chrome, codeLayout, imageLayout, hostLayout, tableLayout, readerChoices,
    )
    val indices: Iterable<Int> = viewportOnlyIndices?.asIterable() ?: (0 until minOf(segmentViews.size, frames.size / 2))
    for (index in indices) {
      val child = segmentViews[index]
      if (child is DeferredTextView) continue
      val top = paddingTop + frames[index * 2]
      val height = frames[index * 2 + 1]
      // A closed details body has a zero frame. Hide its native child as well,
      // so it cannot draw or expose accessibility content outside that frame.
      // Hosted children remain GONE because their React slot owns the drawing.
      if (height <= 0 && child.visibility != View.GONE) child.visibility = View.INVISIBLE
      else if (height > 0 && child.visibility == View.INVISIBLE) child.visibility = View.VISIBLE
      // Preserve child requests for text, controls and scroll range changes.
      // Settled siblings with identical exact bounds need no native layout pass.
      val requested = child.isLayoutRequested
      if (requested || child.measuredWidth != width || child.measuredHeight != height) {
        child.measure(
          MeasureSpec.makeMeasureSpec(width, MeasureSpec.EXACTLY),
          MeasureSpec.makeMeasureSpec(height, MeasureSpec.EXACTLY),
        )
      }
      if (requested || child.isLayoutRequested || child.left != paddingLeft || child.top != top ||
        child.right != paddingLeft + width || child.bottom != top + height) {
        child.layout(paddingLeft, top, paddingLeft + width, top + height)
      }
    }
  }

  override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
    val width = MeasureSpec.getSize(widthMeasureSpec)
    val height = MeasureSpec.getSize(heightMeasureSpec)
    // The segment layer spans the view, and the host layer spans it too: Fabric
    // has already given every hosted child its own frame, so neither layer lays
    // its children out. Only the document's own height is measured here, which is
    // what Fabric asked for.
    segmentLayer.measure(
      MeasureSpec.makeMeasureSpec(width, MeasureSpec.EXACTLY),
      MeasureSpec.makeMeasureSpec(height, MeasureSpec.EXACTLY),
    )
    hostLayer.measure(
      MeasureSpec.makeMeasureSpec(width, MeasureSpec.EXACTLY),
      MeasureSpec.makeMeasureSpec(height, MeasureSpec.EXACTLY),
    )
    setMeasuredDimension(width, height)
  }

  override fun onLayout(changed: Boolean, left: Int, top: Int, right: Int, bottom: Int) {
    // The segment layer fills the view, and the host layer fills it above the
    // segments. A touch no hosted child takes falls through to the segments
    // below, because a ViewGroup offers an unhandled event to the next child
    // down rather than swallowing it.
    segmentLayer.layout(paddingLeft, paddingTop, right - paddingRight, bottom - paddingBottom)
    hostLayer.layout(0, 0, right - left, bottom - top)
    syncSegmentViews()

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
        syncSegmentViews()
      }
    }
  }
}

/**
 * The layer the segment views live in.
 *
 * It exists so the two child reconciliations in this view stay apart: this one
 * reconciles by document index, Fabric's reconciles the host children in React's
 * order. Sharing one list of children would have each deleting the other's views.
 */
internal class SegmentLayer(context: Context) : ViewGroup(context) {
  override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
    setMeasuredDimension(MeasureSpec.getSize(widthMeasureSpec), MeasureSpec.getSize(heightMeasureSpec))
  }

  override fun onLayout(changed: Boolean, left: Int, top: Int, right: Int, bottom: Int) {
    // Every child is laid out by the view that owns the document, at the frame
    // the measurement stacked, so this layer only has to hand them through.
    for (index in 0 until childCount) {
      val child = getChildAt(index)
      if (child.isLayoutRequested) child.layout(child.left, child.top, child.right, child.bottom)
    }
  }

  override fun shouldDelayChildPressedState(): Boolean = false
}

/**
 * Where Fabric mounts a hosted React child, above the segments.
 *
 * Its `onLayout` does nothing, as `ReactViewGroup`'s does: the child's frame came
 * from the shadow node's measurement, and re-laying it out here would put it
 * somewhere the document was not measured for.
 */
internal class HostLayer(context: Context) : ViewGroup(context) {
  init {
    // A layer with no children must not swallow touches meant for the document
    // beneath it, and one with children must not clip them.
    clipChildren = false
    clipToPadding = false
  }

  override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
    setMeasuredDimension(MeasureSpec.getSize(widthMeasureSpec), MeasureSpec.getSize(heightMeasureSpec))
  }

  override fun onLayout(changed: Boolean, left: Int, top: Int, right: Int, bottom: Int) {
    // Nothing: every child's frame was set by Fabric.
  }

  override fun shouldDelayChildPressedState(): Boolean = false
}
