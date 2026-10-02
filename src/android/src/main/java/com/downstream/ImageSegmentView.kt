package com.downstream

import android.content.Context
import android.graphics.drawable.GradientDrawable
import android.view.Gravity
import android.widget.FrameLayout
import android.widget.ImageView
import android.widget.TextView
import com.facebook.react.bridge.ReactContext

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
  private val photo = ImageView(context).apply {
    scaleType = ImageView.ScaleType.FIT_CENTER
  }
  private val altText = TextView(context).apply {
    includeFontPadding = false
    gravity = Gravity.CENTER_VERTICAL
  }

  private var segment: ImageSegment? = null
  private var metrics = ImageMetrics()
  private var layout = ImageLayout()
  private var columnWidth = 0

  /** Set by the view group: reports the answer so the shadow node can record it. */
  var onImageAnswer: ((ordinal: Int, state: ImageState) -> Unit)? = null

  /** Set by the view group: reports a press. */
  var onImagePress: ((url: String, alt: String, title: String, ordinal: Int) -> Unit)? = null

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
    altText.text = value.alt
    // The alt text is the label, and the URL is not: a screen reader announcing a
    // picture's address tells the reader nothing about the picture.
    contentDescription = value.alt.ifEmpty { null }
    applyState()
    requestLayout()
  }

  /**
   * Starts, replaces, or abandons the one request this view may have.
   *
   * Keyed by the URL and the size it was asked for, so a recycled view does not
   * restart a load that already finished and does not keep a load whose size the
   * theme has since changed. Four requests is the engine's limit across the whole
   * document, and a view that must not start one says so here rather than by
   * failing later.
   */
  private fun applyState() {
    val current = segment ?: return
    val state = layout.state.find(current.ordinal)?.state ?: ImageState.PENDING
    val mayLoad = layout.state.enabled && current.loadable &&
      state != ImageState.BLOCKED && state != ImageState.FAILED && state != ImageState.DISABLED

    photo.visibility = if (state == ImageState.LOADED) VISIBLE else GONE
    placeholder.visibility = if (state == ImageState.LOADED || !mayLoad) GONE else VISIBLE
    // Alt text is what the reader gets, so it is shown rather than an empty box.
    // An image with no alt text shows nothing rather than a bare filename.
    altText.visibility = if (altText.text.isNullOrEmpty() || mayLoad) GONE else VISIBLE

    if (state == ImageState.LOADED || !mayLoad || columnWidth <= 0) {
      return
    }
    val box = imageBox(layout.state.find(current.ordinal), columnWidth.toFloat(), metrics)
    AndroidImages.load(
      reactContext = context as? ReactContext ?: return,
      url = current.url,
      width = box.width.toInt(),
      height = box.height.toInt(),
      // The answer is reported, not applied: the state belongs to the engine, and a
      // view that wrote its own height would be measuring against itself.
      onAnswer = { loaded ->
        onImageAnswer?.invoke(current.ordinal, if (loaded) ImageState.LOADED else ImageState.FAILED)
      },
    )
  }

  override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
    val available = MeasureSpec.getSize(widthMeasureSpec)
    val current = segment
    if (current == null || available <= 0) {
      setMeasuredDimension(available.coerceAtLeast(0), 0)
      return
    }
    columnWidth = available
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
  }

  override fun onLayout(changed: Boolean, left: Int, top: Int, right: Int, bottom: Int) {
    val width = right - left - paddingLeft - paddingRight
    val height = bottom - top - paddingTop - paddingBottom
    for (child in listOf(placeholder, photo, altText)) {
      child.layout(paddingLeft, paddingTop, paddingLeft + width, paddingTop + height)
    }
  }

  override fun onDetachedFromWindow() {
    super.onDetachedFromWindow()
    // A recycled view stops caring about the answer, so a load that finishes after
    // this cannot write into a view the list has already reused.
    onImageAnswer = null
    onImagePress = null
  }

  init {
    addView(placeholder)
    addView(altText)
    addView(photo)
    setOnClickListener {
      val current = segment ?: return@setOnClickListener
      onImagePress?.invoke(current.url, current.alt, current.title, current.ordinal)
    }
    // A picture inside a link is a link, and the view group routes that press to
    // the link before it reaches here, so reaching here means there was no link.
    isClickable = true
  }
}