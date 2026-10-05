package com.downstream

import kotlin.math.min

/**
 * The Kotlin mirror of `src/cpp/downstream/ImageLayout.h`, which is the one
 * definition of how an image takes up room in a column.
 *
 * The shadow node measures with the C++ `imageBox` and the view draws with the
 * one here, so the two copies have to answer every question the same way.
 * `imageBox` in `src/cpp/tests/test_images.cpp` and `imageBox` below are run
 * against one table by `pnpm test:kotlin`, which is what makes a divergence a
 * failing test rather than an image that is 96 points taller on one platform.
 */
internal enum class ImageState(val code: Int) {
  /**
   * The destination is still arriving, or no request has started. The
   * placeholder box.
   */
  PENDING(0),

  /** A request is in flight and its size is not known yet. */
  LOADING(1),

  /** Loaded, with a size. */
  LOADED(2),

  /**
   * The request failed, or the bytes are not something the image pipeline
   * decodes, which for SVG means the same thing.
   */
  FAILED(3),

  /** The prefix list does not allow this URL, so no request was ever made. */
  BLOCKED(4),

  /**
   * The `images` prop is off, or the block has not settled, so an unfinished
   * destination is not yet a request.
   */
  DISABLED(5);

  companion object {
    fun of(code: Int): ImageState? = entries.firstOrNull { it.code == code }
  }
}

/** One image's state in one view. */
internal data class ImageEntry(
  /** The image's index in the document's image list. */
  val ordinal: Int,
  val state: ImageState = ImageState.PENDING,
  /**
   * The image's own pixels. [ImageState.LOADED] only, and never scaled: the point
   * size is derived from it, and the same image measured twice has to measure the
   * same both times.
   */
  val pixelWidth: Float = 0f,
  val pixelHeight: Float = 0f,
)

/**
 * Every image answer one view has: whether images are on, and each image's state
 * and size.
 *
 * This is what the mounting state carries and what the view draws from, so it is
 * exactly what both need. [entries] is sorted by ordinal, which is what lets a
 * caller binary-search for the entry it wants, the same contract
 * `hasChoice` in `CodeLayout.h` has.
 */
internal data class ImageStateLayout(
  /** The `images` prop. False means every image draws its alt text. */
  val enabled: Boolean = true,
  val entries: List<ImageEntry> = emptyList(),
) {
  /** The entry for [ordinal], or null when this view has no answer for it. */
  fun find(ordinal: Int): ImageEntry? {
    var low = 0
    var high = entries.size - 1
    while (low <= high) {
      val middle = low + (high - low) / 2
      val entry = entries[middle]
      when {
        entry.ordinal < ordinal -> low = middle + 1
        entry.ordinal > ordinal -> high = middle - 1
        else -> return entry
      }
    }
    return null
  }
}

/**
 * What measurement needs: the answers, plus the policy that produces them.
 *
 * The prefixes live here rather than in the state because only a measurement has
 * to decide whether a URL may be fetched — the view is handed a state that
 * already says [ImageState.BLOCKED] — so the round trip through JNI carries
 * entries alone. Changing the prefix list therefore changes an entry, which is
 * what makes the shadow node notice.
 */
internal data class ImageLayout(
  val state: ImageStateLayout = ImageStateLayout(),
  val allowedPrefixes: List<String> = emptyList(),
)

/**
 * The placeholder box, as Streamdown's `h-24 w-48`: 96 by 192 points at 16 points
 * to the rem, which is a phone's default root font size. Mirrors
 * `kImagePlaceholderWidth` and `kImagePlaceholderHeight`.
 */
internal const val kImagePlaceholderWidth = 192f

internal const val kImagePlaceholderHeight = 96f

/**
 * One line of fallback text at a platform's own line height, so a broken image
 * takes the room of a line of prose and the text after it does not move. Mirrors
 * `kImageFallbackLineHeight`.
 */
internal const val kImageFallbackLineHeight = 18f

/**
 * The theme's image metrics, in pixels.
 *
 * The values that change an image's box. They come from the theme rather than
 * from props, so an app restyles an image without a props change, and a view
 * that passes no theme gets these defaults.
 */
internal data class ImageMetrics(
  /** A cap on a loaded image's height. 0, the default, is no cap. */
  val maxHeight: Float = 0f,
  /** The corner radius. Drawn, never measured. */
  val borderRadius: Float = 8f,
  val marginTop: Float = 0f,
  val marginBottom: Float = 0f,
  /** One line of fallback text. */
  val fallbackTextHeight: Float = kImageFallbackLineHeight,
  /** The placeholder's size, clamped to the column. */
  val placeholderWidth: Float = 192f,
  val placeholderHeight: Float = 96f,
)

/**
 * The box one image takes, as a width and a height.
 *
 * Named rather than reusing a size class, because two types with one name make
 * every unqualified use ambiguous.
 */
internal data class ImageBox(val width: Float, val height: Float)

/** The scale that fits [size] into [limit], never more than 1. */
private fun fitScale(size: Float, limit: Float): Float =
  if (!(size > 0f) || !(limit > 0f) || size <= limit) 1f else limit / size

/**
 * The box one image takes in a column, from its state and the theme's metrics.
 *
 * The mirror of `downstream::imageBox`, and the only place an image's height is
 * decided on this platform. The shadow node measures with the C++ copy, so the
 * two are held to one table by `pnpm test:kotlin`.
 *
 * A pending or loading image with no known size is the placeholder, clamped to
 * the column so a narrow column never gets a box wider than itself. A loaded
 * image is its own pixels, scaled down to the column and to [ImageMetrics.maxHeight]
 * and never up: an image smaller than the column is not blown up, because a
 * 64-pixel icon at four times its size is a blur and not the picture. A failed,
 * blocked, or disabled image is one line of fallback text, so a broken image takes
 * the room of a line rather than of a placeholder the reader cannot use.
 */
internal fun imageBox(entry: ImageEntry?, columnWidth: Float, metrics: ImageMetrics): ImageBox {
  val column = if (columnWidth > 0f) columnWidth else 0f

  if (entry == null) {
    // No answer for this image yet, which is a view handed a document it has not
    // read. The placeholder is the honest answer: it is what the reader would see
    // a frame later.
    return ImageBox(min(metrics.placeholderWidth, column), min(metrics.placeholderHeight, column))
  }

  when (entry.state) {
    ImageState.PENDING, ImageState.LOADING -> {
      // A size known before the bytes arrive is as good as a loaded size: the
      // cache can hold a size for a URL this view has never loaded, which is what
      // makes a scrolled-back message lay out at its final height on its first
      // frame.
      if (!(entry.pixelWidth > 0f && entry.pixelHeight > 0f)) {
        return ImageBox(min(metrics.placeholderWidth, column), min(metrics.placeholderHeight, column))
      }
    }
    ImageState.FAILED, ImageState.BLOCKED, ImageState.DISABLED ->
      // One line of prose, so the text after a broken image does not move when
      // the load fails and the placeholder collapses.
      return ImageBox(column, metrics.fallbackTextHeight)
    ImageState.LOADED -> Unit
  }

  val width = if (entry.pixelWidth > 0f) entry.pixelWidth else metrics.placeholderWidth
  val height = if (entry.pixelHeight > 0f) entry.pixelHeight else metrics.placeholderHeight
  val scale = min(fitScale(width, column), fitScale(height, metrics.maxHeight))
  return ImageBox(width * scale, height * scale)
}

/**
 * The state one image takes, given this view's policy and what the engine knows
 * about the URL. The mirror of `downstream::imageStateFor`.
 *
 * [settled] is the document's own: a destination that is still arriving is
 * [ImageState.PENDING], and a settled empty one is simply not loadable. That is
 * the only difference between "the writer has not finished typing the URL" and
 * "the writer wrote no URL", and it is why an empty image at the end of a
 * finished stream shows its alt text rather than a box that never fills.
 */
internal fun imageStateFor(
  enabled: Boolean,
  urlLoadable: Boolean,
  settled: Boolean,
  known: Boolean,
): ImageState = when {
  !enabled -> ImageState.DISABLED
  !urlLoadable -> if (settled) ImageState.DISABLED else ImageState.PENDING
  else -> if (known) ImageState.LOADED else ImageState.LOADING
}

/**
 * The lowercase of [codePoint], folding ASCII only.
 *
 * ASCII only because the C++ side folds with `std::tolower` on bytes, and
 * folding one byte of a multi-byte character would be a rule neither side could
 * explain. A non-ASCII prefix therefore compares exactly, which for two UTF-8
 * strings is the same answer as comparing bytes.
 */
private fun asciiLower(codePoint: Int): Int =
  if (codePoint in 'A'.code..'Z'.code) codePoint + ('a'.code - 'A'.code) else codePoint

/**
 * True when [text] starts with [prefix], ignoring case.
 *
 * Compared code point by code point rather than UTF-16 unit, which would split a
 * surrogate pair and fold each half on its own.
 */
private fun startsWithIgnoringCase(text: String, prefix: String): Boolean {
  var i = 0
  var j = 0
  while (j < prefix.length) {
    if (i >= text.length) {
      return false
    }
    val textPoint = asciiLower(text.codePointAt(i))
    val prefixPoint = asciiLower(prefix.codePointAt(j))
    if (textPoint != prefixPoint) {
      return false
    }
    i += Character.charCount(textPoint)
    j += Character.charCount(prefixPoint)
  }
  return true
}

/**
 * Whether [url] may be fetched, under [prefixes]. The mirror of
 * `downstream::imageUrlAllowed`.
 *
 * rehype-harden's rule, which is also Streamdown's: `*` allows any host, and
 * otherwise a URL matches when it starts with one of the prefixes, compared
 * case-insensitively. A prefix list with no `*` and no entries allows nothing,
 * because "fetch from nowhere" is the only safe reading of a list an app got
 * wrong.
 */
internal fun imageUrlAllowed(url: String, prefixes: List<String>): Boolean =
  prefixes.any { it == "*" || (it.isNotEmpty() && startsWithIgnoringCase(url, it)) }
