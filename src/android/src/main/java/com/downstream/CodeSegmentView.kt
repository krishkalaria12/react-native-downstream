package com.downstream

import android.content.Context
import android.content.res.ColorStateList
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.os.Handler
import android.os.Looper
import android.text.Layout
import android.text.TextPaint
import android.text.TextUtils
import android.util.TypedValue
import android.view.Gravity
import android.view.MotionEvent
import android.view.View
import android.widget.FrameLayout
import android.widget.HorizontalScrollView
import android.widget.ImageButton
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView

/** How far a line number is dimmed below the code it numbers. Mirrors `kGutterAlpha` on iOS. */
private const val GUTTER_ALPHA = 0.5f

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

  private val markPaint = Paint()

  /**
   * The line bands a fence asked for with `{1,3-5}` in its info string.
   *
   * Drawn behind the code rather than beside it, and behind *every* visual line a
   * logical line wraps onto, because a mark is about a line of the file rather
   * than about a line of the screen: marking only the first visual row would leave
   * the rest of a wrapped marked line looking unmarked.
   *
   * No height of its own, so it is not a layout input and does not join
   * `LayoutHint` — the bands are drawn into the card's existing rows.
   */
  private fun drawLineMarks(canvas: Canvas, textLayout: Layout) {
    val segment = segment ?: return
    val marks = segment.fenceMeta.marks
    if (marks.isEmpty()) return
    val color = segment.colors.slotOrNull(ThemeSlots.SLOT_CODE_BLOCK_LINE_MARK_COLOR) ?: return
    val first = segment.fenceMeta.startLine
    val starts = segment.lineStarts
    val dirty = canvas.clipBounds
    markPaint.color = color
    for (mark in marks) {
      // Marked lines are the fence's own numbering, so a fence that started at
      // line 10 marks line 10 first — which is what makes `{1,3-5}` on a diff mean
      // the same thing as it does in every other diff viewer.
      val from = (mark[0] - first).toInt()
      val to = (mark[1] - first).toInt()
      for (index in from..to) {
        if (index < 0 || index >= starts.size) continue
        val offset = starts[index].coerceAtMost(textLayout.text.length)
        val lastOffset = (offset + 1).coerceAtMost(textLayout.text.length)
        for (line in textLayout.getLineForOffset(offset)..
          textLayout.getLineForOffset(lastOffset)) {
          val top = textLayout.getLineTop(line)
          if (top > dirty.bottom) break
          if (textLayout.getLineBottom(line) < dirty.top) continue
          canvas.drawRect(0f, top.toFloat(), width.toFloat(), textLayout.getLineBottom(line).toFloat(), markPaint)
        }
      }
    }
  }

  override fun onDraw(canvas: Canvas) {
    val segment = this.segment ?: return
    val layout = this.layout ?: return
    val body = this.body ?: return
    val textLayout = body.layout ?: return
    drawLineMarks(canvas, textLayout)
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
    // A number fades with its line's first characters, so a code block whose words
    // arrive one at a time has its numbers arrive with them rather than all at
    // once beside text that is still invisible. The band is asked once rather than
    // per line: it is the same answer for every number, and a fence with two
    // thousand lines must not walk it two thousand times.
    val baseAlpha = numberPaint.alpha
    for (index in first until starts.size) {
      val offset = starts[index].coerceAtMost(textLayout.text.length)
      val line = textLayout.getLineForOffset(offset)
      val baseline = textLayout.getLineBaseline(line)
      if (textLayout.getLineTop(line) > dirty.bottom) break
      val number = (layout.firstNumber + index).toString()
      // One alpha for the whole number, so a line's number is a single value rather
      // than a gradient, which is what a reader would expect of a number.
      numberPaint.alpha = (baseAlpha * body.alphaAt(offset)).toInt().coerceIn(0, 255)
      // Right-aligned against the gap, so the numbers line up on their last digit
      // as they do in GitHub and VS Code.
      canvas.drawText(
        number,
        width - segment.gutterGap - numberPaint.measureText(number),
        baseline.toFloat(),
        numberPaint,
      )
    }
    numberPaint.alpha = baseAlpha
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
  /** True while a React child draws this block instead of the card. */
  private var hosted = false
  private var boundSegment: CodeSegment? = null
  /** Whether the chrome asks for a header row, apart from whether it is hosted. */
  private var headerWanted = true
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

  private val copyButton = controlButton(controlDrawable("copy", R.drawable.ds_ic_copy), LABEL_COPY_CODE)
  private val downloadButton = controlButton(controlDrawable("download", R.drawable.ds_ic_download), LABEL_DOWNLOAD_FILE)
  private val wrapButton = controlButton(controlDrawable("wrap", R.drawable.ds_ic_wrap), LABEL_WRAP_CODE)
  private val expandButton = controlButton(controlDrawable("expand", R.drawable.ds_ic_expand), LABEL_EXPAND_CODE)

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

  fun setSegment(segment: CodeSegment, rendered: RenderedDocument, firstBlock: Int) {
    this.rendered = rendered
    // The body's reveal units name a document block, and a card is reused as its
    // block moves, so the offset is set on every snapshot, not only a new segment.
    body.setBlockOffset(firstBlock)
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
    if (!hosted) bindFallback()
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

  /**
   * Draw this block as a host segment: its container chrome and nothing else.
   *
   * A React child sits on top of it, sized by the measurement, so the card's
   * background, header, gutter and body would be a second drawing of the same
   * space. What stays is what the child cannot draw: the quote and admonition
   * bars beside it and the list marker on its first line, over the full height
   * the child measured.
   */
  fun setHosted(hosted: Boolean) {
    if (this.hosted == hosted) return
    this.hosted = hosted
    // The fallback is bound synchronously before it becomes visible, including
    // failure/suspension/unmount of a hosted child.
    if (!hosted) bindFallback() else {
      body.clearContent()
      boundSegment = null
    }
    applyHeaderVisibility()
    viewport.visibility = if (hosted) View.GONE else View.VISIBLE
    gutter.visibility = if (hosted) View.GONE else View.VISIBLE
    cardBackground.setColor(if (hosted) android.graphics.Color.TRANSPARENT else segment?.colors?.codeBlockBackground ?: 0)
    requestLayout()
    invalidate()
  }

  private fun bindFallback() {
    val segment = segment ?: return
    val rendered = rendered ?: return
    if (boundSegment === segment) return
    body.setContent(segment.text, segment.accessibleBlocks, rendered)
    boundSegment = segment
  }

  fun setHeaderVisible(visible: Boolean) {
    headerWanted = visible
    applyHeaderVisibility()
  }

  /**
   * The header row shows when the chrome asks for it and the block is a card.
   *
   * Two inputs rather than one flag that each setter overwrites: the chrome is
   * synced after the hosted state on every snapshot, and a hosted block's header
   * shown again by that sync draws a card's title row above a chart, while a
   * block that stops being hosted has to get back the header the chrome wants.
   */
  private fun applyHeaderVisibility() {
    val next = if (headerWanted && !hosted) View.VISIBLE else View.GONE
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
    copyButton.setImageResource(
      if (copied) controlDrawable("check", R.drawable.ds_ic_check)
      else controlDrawable("copy", R.drawable.ds_ic_copy),
    )
    copyButton.contentDescription = controlLabels[if (copied) LABEL_COPIED else LABEL_COPY_CODE]
    val wraps = layout?.wraps ?: false
    wrapButton.isSelected = wraps
    wrapButton.contentDescription = controlLabels[if (wraps) LABEL_UNWRAP_CODE else LABEL_WRAP_CODE]
    wrapButton.stateDescription = wrapButton.contentDescription
    expandButton.isSelected = expanded
    expandButton.setImageResource(
      if (expanded) controlDrawable("collapse", R.drawable.ds_ic_collapse)
      else controlDrawable("expand", R.drawable.ds_ic_expand),
    )
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

  override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
    val rendered = this.rendered
    val traceId = if (rendered?.tracing == true) rendered.documentId else 0L
    if (traceId != 0L) DownstreamNative.traceDocument(traceId, "android_code_measure_start")
    try {
      super.onMeasure(widthMeasureSpec, heightMeasureSpec)
    } finally {
      if (traceId != 0L) DownstreamNative.traceDocument(traceId, "android_code_measure_end")
    }
  }

  override fun onLayout(changed: Boolean, left: Int, top: Int, right: Int, bottom: Int) {
    val segment = this.segment ?: return
    val rendered = this.rendered ?: return
    val layout = this.layout ?: return
    val traceId = if (rendered.tracing) rendered.documentId else 0L
    if (traceId != 0L) DownstreamNative.traceDocument(traceId, "android_code_layout_start")
    try {
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
    } finally {
      if (traceId != 0L) DownstreamNative.traceDocument(traceId, "android_code_layout_end")
    }
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
    // A host segment has no header, so its chrome starts at the top of its own
    // frame and spans the height the React component measured.
    val headerTop = if (hosted) 0f else headerHeight().toFloat()
    // The card's fill, border, header and list marker fade with the block's first
    // unit, so a card arrives with its code rather than appearing around text that
    // is still invisible. The body fades through its own units, so the card does
    // not multiply them: the two are read separately and neither is a multiple of
    // the other. A hosted block does not fade at all: the React child over it is
    // not faded by the reveal, so its bars and marker arrive with the component
    // rather than with code text the reader never sees.
    val chrome = if (hosted) 1f else body.alphaAt(0)
    val checkpoint = if (chrome < 1f) canvas.saveLayerAlpha(null, (chrome * 255f).toInt()) else -1
    // A hosted fence draws no card: the component owns its own chrome, and a card
    // behind it would be a second answer to the same block.
    if (!hosted) {
      // The card starts below the header, which sits on the page, as on iOS.
      cardBackground.setBounds(segment.contentLeading, headerTop.toInt(), width, height)
      cardBackground.draw(canvas)
      // The header row's own fill and rule, drawn under its buttons and label.
      segment.colors.headerBackground?.let { fill ->
        barPaint.color = fill
        canvas.drawRect(0f, 0f, width.toFloat(), headerTop, barPaint)
      }
      if (segment.colors.headerBorderColor != null) {
        barPaint.color = segment.colors.headerBorderColor
        canvas.drawRect(0f, headerTop - 1f, width.toFloat(), headerTop, barPaint)
      }
    }
    val firstLine = segment.accessibleBlocks.firstOrNull()
    for (container in segment.containers) {
      val bar = container.barColor
      // A zero-width bar is a bar the theme removed.
      if (bar != null && container.barWidth > 0f) {
        barPaint.color = bar
        val x = if (segment.rtl) width - container.leading - container.barWidth - 2f else container.leading + 2f
        canvas.drawRect(x, headerTop, x + container.barWidth, height.toFloat(), barPaint)
      }
      val marker = container.marker
      if (marker != null && firstLine != null) {
        // The marker's own family and weight, at the size the theme sets.
        val markerPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
          typeface = segment.markerFace
          textSize = segment.markerSize
        }
        // A hosted block's marker sits at the top of its own height, beside the
        // component, rather than beside the first line of code that is not there.
        val baseline = headerTop + (if (hosted) 0f else segment.padding.toFloat()) + markerPaint.fontMetrics.ascent
        val x = if (segment.rtl) {
          container.leading + container.markerGap + markerPaint.measureText(marker)
        } else {
          container.leading + container.width - container.markerGap - markerPaint.measureText(marker)
        }
        canvas.drawText(marker, x, baseline.toFloat(), markerPaint)
      }
    }
    if (checkpoint >= 0) {
      canvas.restoreToCount(checkpoint)
    }
  }
}
