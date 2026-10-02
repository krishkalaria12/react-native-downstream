package com.downstream

import android.net.Uri
import android.os.Handler
import android.os.Looper
import com.facebook.common.references.CloseableReference
import com.facebook.drawee.backends.pipeline.Fresco
import com.facebook.imagepipeline.common.ResizeOptions
import com.facebook.imagepipeline.image.CloseableImage
import com.facebook.imagepipeline.request.ImageRequest
import com.facebook.imagepipeline.request.ImageRequestBuilder
import com.facebook.react.bridge.ReactContext

/**
 * Loading a picture through the app's own pipeline.
 *
 * Every load in the library goes through here rather than through a private HTTP
 * stack, because an app that configured Fresco configured it for a reason: its
 * network detector avoids a request on a metered connection, its disk cache is
 * already warm, and its custom URL handlers know about hosts the library does not.
 * A second fetcher would quietly do none of that, and would be the one part of this
 * library an app could not fix from its own configuration.
 *
 * `Fresco.getImagePipeline` is what makes that true rather than aspirational: it is
 * the pipeline the React Native host initialized with the app's configuration, not
 * one this library builds.
 *
 * This is the only file that names the platform's image APIs, which is the point.
 * Those calls have moved between Fresco and React Native releases — `ImageRequest`'s
 * builder left the class, and `Fresco` moved to the Drawee package — and one file to
 * re-check against a release is easier to keep right than a loader threaded through
 * every segment view.
 *
 * The engine's own limit of four loads applies above this: [load] starts one request
 * and reports its answer, and the shadow node decides whether the fifth picture in a
 * message gets one at all.
 */
internal object AndroidImages {
  /** Results on the main thread, because every caller draws. */
  private val main = Handler(Looper.getMainLooper())

  /**
   * Fetches [url] decoded at [width] by [height], and reports whether it decoded.
   *
   * The size is the box's, not the screen's: a 4,000-pixel photo drawn in a
   * 400-pixel box does not need four thousand pixels of bitmap, and decoding all of
   * them is what makes a message list stutter while it scrolls. That is
   * `ResizeOptions` rather than a resize afterwards, so the decode itself is bounded.
   *
   * [onAnswer] runs on the main thread. It reports the answer and nothing else: the
   * new state is written into the layout map by the engine, and the view is rebuilt
   * from that rather than told to resize itself, which is what keeps a measured
   * height and a drawn one the same number.
   *
   * Reports false for anything that is not a decoded image, which includes SVG:
   * neither platform's pipeline draws it, and an image that cannot be drawn is a
   * failed image, which is a state the box rule already has.
   */
  fun load(
    reactContext: ReactContext,
    url: String,
    width: Int,
    height: Int,
    onAnswer: (loaded: Boolean) -> Unit,
  ) {
    // `getImagePipeline` throws rather than returning null when Fresco was never
    // initialized, which cannot happen in a running app but can happen in a test
    // harness or an early attach. A picture that cannot be fetched is a failed
    // picture, so that is what is reported.
    val pipeline = try {
      Fresco.getImagePipeline()
    } catch (error: IllegalStateException) {
      main.post { onAnswer(false) }
      return
    }

    // `parse` returns null for a string that is not a URI at all, which the engine's
    // own policy already rejects but a hand-built state map could still carry.
    val uri = Uri.parse(url) ?: run {
      main.post { onAnswer(false) }
      return
    }

    val request: ImageRequest = ImageRequestBuilder.newBuilderWithSource(uri)
      .setResizeOptions(ResizeOptions.forDimensions(width.coerceAtLeast(1), height.coerceAtLeast(1)))
      .build()

    // No caller context: the cache key is then the URL and the size alone, which is
    // what makes two views showing the same picture at the same size share one
    // decode.
    val data = pipeline.fetchDecodedImage(request, null)

    // The result is non-null once the request has finished and null while it is in
    // flight or when it failed. Nothing waits on either case: the shadow node writes
    // the answer and rebuilds the view, so this call only has to produce one number.
    val decoded = data.result
    // `CloseableReference` is a counted handle the pipeline owns. It is released
    // straight away because nothing here keeps the bitmap: the ImageView asks the
    // pipeline for the picture itself, and holding the handle would keep a full
    // bitmap alive per message in a scrolling list.
    val size = decoded?.let { sizeOf(it) }
    decoded?.close()
    main.post { onAnswer(size != null) }
  }

  /** The decoded image's own size, or null when it is not something either can draw. */
  private fun sizeOf(reference: CloseableReference<CloseableImage>): Pair<Int, Int>? {
    val image = reference.get() ?: return null
    val width = image.width
    val height = image.height
    return if (width > 0 && height > 0) width to height else null
  }
}