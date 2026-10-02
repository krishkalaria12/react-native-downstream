package com.downstream

import android.content.Context
import android.content.res.ColorStateList
import android.content.res.Configuration
import android.graphics.drawable.GradientDrawable
import android.graphics.drawable.RippleDrawable
import android.text.TextUtils
import android.view.Gravity
import android.view.View
import android.widget.FrameLayout
import android.widget.ImageButton
import android.widget.ImageView
import android.widget.TextView
import com.facebook.drawee.view.SimpleDraweeView

/**
 * One picture, as a view.
 *
 * A view rather than a span because the bytes come from the app's own image
 * pipeline: the loader is the same one React Native's own `<Image>` uses, so the
 * app's Fresco configuration, its network detector, and its disk cache all apply to
 * a picture a model wrote. Resolving takes a background thread and then needs a
 * surface of its own, which is what an `ImageView` is.
 *
 * The box is [imageBox] under whatever answer this view holds, which is the same
 * call the shadow node measured with. That is the whole reason the view holds an
 * [ImageLayout] rather than a bitmap: a load that finishes changes an entry, the
 * box changes, and the frames rebuild around it.
 *
 * A press is reported rather than acted on, for the same reason iOS reports it: the
 * library opens nothing, because an app that has a viewer wants its own, and one
 * that does not should not have a picture that navigates somewhere surprising when
 * tapped.
 */
internal class ImageSegmentView(context: Context) : FrameLayout(context) {
  private val placeholder = View(context)
  /**
   * The picture itself, as a Drawee view.
   *
   * Drawee rather than an `ImageView` because Drawee *is* the app's pipeline: it
   * fetches through the pipeline Fresco was initialized with, hands frames to the
   * GPU as they arrive, and cancels and retries on its own. An `ImageView` would
   * need a bitmap decoded here and would bypass all of it, which is the one thing
   * this library must not do to a picture a model wrote.
   */
  private val photo = SimpleDraweeView(context).apply {
    // Drawee draws into an ImageView, so `scaleType` is the scale type here too,
    // and FIT_CENTER is the one that matches `imageBox`: a picture is fitted to its
    // box and never cropped or stretched out of it.
    scaleType = ImageView.ScaleType.FIT_CENTER
  }
  // One line, because the box the engine reserves for a fallback is one line tall.
  private val altText = TextView(context).apply {
    includeFontPadding = false
    gravity = Gravity.CENTER_VERTICAL
    maxLines = 1
    ellipsize = TextUtils.TruncateAt.END
  }
  private val download = newControlButton(context, R.drawable.ds_ic_download).apply {
    visibility = GONE
    styleDownload(this, context.resources.configuration)
    setOnClickListener {
      val current = segment ?: return@setOnClickListener
      if (visibility == VISIBLE) onImageDownload?.invoke(current)
    }
  }

  private var segment: ImageSegment? = null
  private var metrics = ImageMetrics()
  private var layout = ImageLayout()
  private var columnWidth = 0

  /**
   * Set by the view group: reports a finished load so the engine can record it.
   * The size is the decoded picture's, in pixels, and null means it failed.
   */
  internal var onImageAnswer: ((url: String, answer: AndroidImages.Answer?) -> Unit)? = null

  /** The one request this view holds, and what it was for. */
  private var ticket: AndroidImages.Ticket? = null
  private var ticketUrl: String? = null

  /** Set by the view group: reports a press. */
  internal var onImagePress: ((url: String, alt: String, title: String, range: IntRange) -> Unit)? = null
  internal var onImageDownload: ((ImageSegment) -> Unit)? = null
  private var controls = 0
  private var controlsEnabled = false

  fun setDownloadControl(controls: Int, enabled: Boolean, label: String) {
    this.controls = controls
    controlsEnabled = enabled
    download.contentDescription = label
    applyState()
  }

  /**
   * The theme's metrics and its alt color.
   *
   * Read here rather than by the view group so the box and the border come from one
   * value: a themed measurement and an unthemed draw that disagreed about a margin
   * is a picture whose box does not line up with the text around it.
   */
  fun setStyle(imageMetrics: ImageMetrics, altColor: Int) {
    if (metrics == imageMetrics) {
      return
    }
    metrics = imageMetrics
    val radius = metrics.borderRadius.toInt()
    placeholder.background = GradientDrawable().apply {
      cornerRadius = radius.toFloat()
      setColor(altColor)
    }
    photo.clipToOutline = radius > 0
    requestLayout()
  }

  private var notAvailableLabel = "Image not available"
  private var blockedLabel = "Image blocked"

  /**
   * What a failed and a blocked picture say under their alt text, from the
   * `translations` the app passed, as iOS shows them.
   */
  fun setStatusLabels(notAvailable: String, blocked: String) {
    if (notAvailableLabel == notAvailable && blockedLabel == blocked) {
      return
    }
    notAvailableLabel = notAvailable
    blockedLabel = blocked
    applyState()
  }

  /** The engine's answers, and the prefixes that produced them. */
  fun setImageLayout(value: ImageLayout) {
    if (layout == value) {
      return
    }
    layout = value
    applyState()
    requestLayout()
  }

  fun setSegment(value: ImageSegment) {
    val previous = segment
    if (previous?.ordinal == value.ordinal && previous.url == value.url) {
      return
    }
    segment = value
    // The alt text is the label, and the URL is not: a screen reader announcing a
    // picture's address tells the reader nothing about the picture.
    contentDescription = value.alt.ifEmpty { null }
    applyState()
    requestLayout()
  }

  /**
   * Shows what the engine's answer says, and starts the one request this view may
   * have.
   *
   * The state is the source of truth, as on iOS: `Pending` and `Loading` draw the
   * placeholder, `Loaded` draws the picture, and every other state draws the alt
   * text. A load starts only once the column is known, because the column is the
   * decode bound, and only once per URL.
   */
  private fun applyState() {
    val current = segment ?: return
    val state = if (!layout.state.enabled) {
      ImageState.DISABLED
    } else {
      layout.state.find(current.ordinal)?.state ?: ImageState.PENDING
    }
    val loading = state == ImageState.PENDING || state == ImageState.LOADING
    photo.visibility = if (state == ImageState.LOADED) VISIBLE else GONE
    val showsDownload = state == ImageState.LOADED && controlsEnabled && controls and CONTROL_IMAGE_DOWNLOAD != 0
    if ((download.visibility == VISIBLE) != showsDownload) {
      download.visibility = if (showsDownload) VISIBLE else GONE
      // The block settles in the parent's own layout pass, and React Native does
      // not run a native subtree's layout again until the next snapshot, so a
      // button that appears then would never get a frame. Its frame depends only
      // on this view's size, so it is placed here.
      placeDownload()
    }
    placeholder.visibility = if (loading) VISIBLE else GONE
    // The alt text, and under it what happened: a reader should be able to tell a
    // picture that failed from one this app chose not to load. A disabled picture
    // is only its alt text, because nothing went wrong.
    val status = when (state) {
      ImageState.FAILED -> notAvailableLabel
      ImageState.BLOCKED -> blockedLabel
      else -> ""
    }
    val fallback = listOf(current.alt, status).filter { it.isNotEmpty() }.joinToString(" · ")
    if (altText.text.toString() != fallback) {
      altText.text = fallback
    }
    altText.visibility = if (loading || state == ImageState.LOADED || fallback.isEmpty()) GONE else VISIBLE

    if (ticketUrl != null && ticketUrl != current.url) {
      AndroidImages.cancel(ticket)
      ticket = null
      ticketUrl = null
    }
    if (columnWidth <= 0) {
      return
    }
    val boundWidth = columnWidth
    val boundHeight = metrics.maxHeight.toInt().takeIf { it > 0 } ?: columnWidth
    if (state == ImageState.LOADED) {
      // The same request the load made, so the Drawee view finds the decode in the
      // memory cache rather than fetching and decoding the picture a second time.
      if (photo.tag != current.url) {
        photo.tag = current.url
        photo.setImageRequest(AndroidImages.imageRequest(current.url, boundWidth, boundHeight))
      }
      return
    }
    if (state != ImageState.LOADING || !current.loadable || ticketUrl == current.url) {
      return
    }
    val url = current.url
    ticketUrl = url
    ticket = AndroidImages.load(url, boundWidth, boundHeight) { answer ->
      ticket = null
      // The answer is reported, not applied: the state belongs to the engine, and a
      // view that wrote its own height would be measuring against itself. The URL
      // is kept until the state says otherwise, so a finished load is not restarted.
      onImageAnswer?.invoke(url, answer)
    }
  }

  override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
    val available = MeasureSpec.getSize(widthMeasureSpec)
    val current = segment
    if (current == null || available <= 0) {
      setMeasuredDimension(available.coerceAtLeast(0), 0)
      return
    }
    if (columnWidth != available) {
      columnWidth = available
      applyState()
    }
    setPadding(0, metrics.marginTop.toInt(), 0, metrics.marginBottom.toInt())
    val box = imageBox(layout.state.find(current.ordinal), available.toFloat(), metrics)
    val height = (box.height + metrics.marginTop + metrics.marginBottom).toInt().coerceAtLeast(0)
    setMeasuredDimension(available, height)
    // Every child fills the box: an `ImageView` scales its own content, and the
    // alt text wraps within the column rather than being clipped to its own ideal.
    for (child in listOf(placeholder, photo, altText)) {
      child.measure(
        MeasureSpec.makeMeasureSpec(available - paddingLeft - paddingRight, MeasureSpec.EXACTLY),
        MeasureSpec.makeMeasureSpec(height - paddingTop - paddingBottom, MeasureSpec.EXACTLY),
      )
    }
    val size = download.layoutParams.width
    download.measure(
      MeasureSpec.makeMeasureSpec(size, MeasureSpec.EXACTLY),
      MeasureSpec.makeMeasureSpec(size, MeasureSpec.EXACTLY),
    )
  }

  override fun onConfigurationChanged(newConfig: Configuration) {
    super.onConfigurationChanged(newConfig)
    styleDownload(download, newConfig)
  }

  /** Measures and places the download button in this view's bottom-right corner. */
  private fun placeDownload() {
    val size = download.layoutParams?.width ?: return
    if (size <= 0 || width <= 0 || height <= 0) {
      return
    }
    download.measure(
      MeasureSpec.makeMeasureSpec(size, MeasureSpec.EXACTLY),
      MeasureSpec.makeMeasureSpec(size, MeasureSpec.EXACTLY),
    )
    val inset = (4 * context.resources.displayMetrics.density).toInt()
    val innerWidth = width - paddingLeft - paddingRight
    val innerHeight = height - paddingTop - paddingBottom
    val x = paddingLeft + (innerWidth - download.measuredWidth - inset).coerceAtLeast(0)
    val y = paddingTop + (innerHeight - download.measuredHeight - inset).coerceAtLeast(0)
    download.layout(x, y, x + download.measuredWidth, y + download.measuredHeight)
    // Laid out outside a layout pass, so nothing would draw it until something
    // else on screen changed.
    download.invalidate()
    invalidate()
  }

  override fun onLayout(changed: Boolean, left: Int, top: Int, right: Int, bottom: Int) {
    val width = right - left - paddingLeft - paddingRight
    val height = bottom - top - paddingTop - paddingBottom
    for (child in listOf(placeholder, photo, altText)) {
      child.layout(paddingLeft, paddingTop, paddingLeft + width, paddingTop + height)
    }
    placeDownload()
  }

  override fun onDetachedFromWindow() {
    super.onDetachedFromWindow()
    // A detached view stops wanting its picture, and the request is closed when no
    // other view wants it. The callbacks stay: the view group set them once, and a
    // view that is attached again must still report.
    AndroidImages.cancel(ticket)
    ticket = null
    ticketUrl = null
    photo.tag = null
  }

  override fun onAttachedToWindow() {
    super.onAttachedToWindow()
    applyState()
  }

  init {
    addView(placeholder)
    addView(altText)
    addView(photo)
    // A clickable child consumes its gesture, so a save never reports an image press.
    addView(download)
    setOnClickListener {
      val current = segment ?: return@setOnClickListener
      onImagePress?.invoke(current.url, current.alt, current.title, current.range)
    }
    // A picture inside a link is a link, and the view group routes that press to
    // the link before it reaches here, so reaching here means there was no link.
    isClickable = true
  }
}

/**
 * The download button's round backing, because the button sits on the picture and
 * a bare glyph disappears on a dark or busy corner. It follows the appearance the
 * way the page does: light on light, dark on dark.
 */
private fun styleDownload(button: ImageButton, configuration: Configuration) {
  val night = (configuration.uiMode and Configuration.UI_MODE_NIGHT_MASK) == Configuration.UI_MODE_NIGHT_YES
  val fill = GradientDrawable().apply {
    shape = GradientDrawable.OVAL
    setColor(if (night) 0xCC1C1C1E.toInt() else 0xE6FFFFFF.toInt())
  }
  button.background = RippleDrawable(ColorStateList.valueOf(if (night) 0x33FFFFFF else 0x22000000), fill, null)
  button.imageTintList = ColorStateList.valueOf(if (night) 0xFFE5E5EA.toInt() else 0xFF3A3A3C.toInt())
}
