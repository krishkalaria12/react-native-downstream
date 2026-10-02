package com.downstream

import android.os.Handler
import android.os.Looper
import com.facebook.imagepipeline.request.ImageRequest
import com.facebook.react.bridge.ReactContext
import com.facebook.react.views.image.ReactImageLoader

/**
 * Loading a picture through the app's own pipeline.
 *
 * Every load in the library goes through here rather than through a private HTTP
 * stack, because an app that configured Fresco configured it for a reason: its
 * network detector avoids a request on a metered connection, its disk cache is
 * already warm, and its custom URL handlers know about hosts the library does not.
 * A second fetcher would quietly do none of that, and would be the one part of
 * this library an app could not fix from its own configuration.
 *
 * This is the only file that names the platform's image APIs, which is the point:
 * that call has moved between React Native releases, and one file to re-check is
 * easier to keep right than a loader threaded through every segment view.
 *
 * The engine's own limit of four loads applies above this: [load] starts one
 * request and reports its answer, and the shadow node decides whether the fifth
 * picture in a message gets one at all.
 */
internal object AndroidImages {
  /** Results on the main thread, because every caller draws. */
  private val main = Handler(Looper.getMainLooper())

  /**
   * Fetches [url] decoded at [width] by [height], and reports whether it decoded.
   *
   * The size is the box's, not the screen's: a 4,000-pixel photo drawn in a
   * 400-pixel box does not need four thousand pixels of bitmap, and decoding all of
   * them is what makes a message list stutter while it scrolls.
   *
   * [onAnswer] runs on the main thread. It reports the answer and nothing else: the
   * new state is written into the layout map by the engine, and the view is rebuilt
   * from that rather than told to resize itself, which is what keeps a measured
   * height and a drawn one the same number.
   *
   * Reports false for anything that is not a decoded bitmap, including SVG: neither
   * platform's pipeline draws it, and an image that cannot be drawn is a failed
   * image, which is a state the box rule already has.
   */
  fun load(
    reactContext: ReactContext,
    url: String,
    width: Int,
    height: Int,
    onAnswer: (loaded: Boolean) -> Unit,
  ) {
    val pipeline = ReactImageLoader.createImageLoader(reactContext).imagePipeline
    val request = ImageRequest.Builder(reactContext)
      .data(url)
      // The decode target rather than a resize afterwards: a photo scaled after the
      // fact is a bitmap at full size for as long as the message is on screen.
      .target(width.coerceAtLeast(1), height.coerceAtLeast(1))
      .build()

    val data = pipeline.fetchImage(request, null, main)
    // `result` is non-null on a completed fetch and null while it is in flight or
    // when it failed. Nothing is subscribed to either case, because the engine's
    // shadow node writes the answer and rebuilds the view; this call only has to
    // produce one number.
    val decoded = data.result
    main.post { onAnswer(decoded != null && decoded.width > 0 && decoded.height > 0) }
  }
}