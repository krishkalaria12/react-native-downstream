package com.downstream

import android.net.Uri
import android.os.Handler
import android.os.Looper
import android.webkit.MimeTypeMap
import com.facebook.common.memory.PooledByteBuffer
import com.facebook.common.executors.UiThreadImmediateExecutorService
import com.facebook.common.references.CloseableReference
import com.facebook.datasource.BaseDataSubscriber
import com.facebook.datasource.DataSource
import com.facebook.drawee.backends.pipeline.Fresco
import com.facebook.imagepipeline.common.ResizeOptions
import com.facebook.imagepipeline.image.CloseableImage
import com.facebook.imagepipeline.request.ImageRequest
import com.facebook.imagepipeline.request.ImageRequestBuilder
import com.facebook.imageformat.ImageFormatChecker
import java.io.ByteArrayInputStream
import java.util.concurrent.Executors

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
 * Those calls have moved between Fresco and React Native releases, and one file to
 * re-check against a release is easier to keep right than a loader threaded through
 * every segment view.
 *
 * The contract's three rules, which iOS's `DSImageLoader` applies too:
 *
 *   - At most four requests in flight, so a paragraph of `![](…)` does not open
 *     thirty sockets to a host the reader never chose.
 *   - One request per URL and size, shared by every view that wants it.
 *   - Every request is held by the views that want it and closed when the last
 *     one lets go, so a recycled message releases what it started.
 *
 * Everything here runs on the main thread: the pipeline delivers to the UI
 * executor, and every caller is a view.
 */
internal object AndroidImages {
  private const val MAX_IN_FLIGHT = 4

  /** One decoded answer: the picture's own size in pixels, or null when it failed. */
  class Answer(val width: Int, val height: Int)

  /** One view's interest in one request, which is what [cancel] takes back. */
  class Ticket internal constructor(internal val key: String)

  private class Request(val key: String, val image: ImageRequest) {
    val waiting = LinkedHashMap<Ticket, (Answer?) -> Unit>()
    var source: DataSource<CloseableReference<CloseableImage>>? = null
  }

  private val main = Handler(Looper.getMainLooper())
  private val live = HashMap<String, Request>()
  private val queued = ArrayDeque<String>()
  private var started = 0
  private val downloads = Executors.newSingleThreadExecutor()

  class Download(val bytes: ByteArray, val mimeType: String)

  /** Saves the original encoded bytes through the host's pipeline, including its cache and URL handlers. */
  fun download(url: String, onAnswer: (Download?) -> Unit) {
    val source = try {
      val request = ImageRequestBuilder.newBuilderWithSource(Uri.parse(url)).build()
      Fresco.getImagePipeline().fetchEncodedImage(request, null)
    } catch (error: RuntimeException) {
      main.post { onAnswer(null) }
      return
    }
    source.subscribe(
      object : BaseDataSubscriber<CloseableReference<PooledByteBuffer>>() {
        override fun onNewResultImpl(dataSource: DataSource<CloseableReference<PooledByteBuffer>>) {
          if (!dataSource.isFinished) return
          val reference = dataSource.result
          val result = try {
            reference?.get()?.let { buffer ->
              if (buffer.size() == 0) return@let null
              val bytes = ByteArray(buffer.size())
              buffer.read(0, bytes, 0, bytes.size)
              val extension = ImageFormatChecker.getImageFormat(ByteArrayInputStream(bytes)).fileExtension
              val mime = extension?.let { MimeTypeMap.getSingleton().getMimeTypeFromExtension(it) }
                ?: "application/octet-stream"
              Download(bytes, mime)
            }
          } catch (error: Exception) {
            null
          } finally {
            CloseableReference.closeSafely(reference)
          }
          main.post { onAnswer(result) }
        }

        override fun onFailureImpl(dataSource: DataSource<CloseableReference<PooledByteBuffer>>) {
          main.post { onAnswer(null) }
        }
      },
      downloads,
    )
  }

  /** The request a Drawee view should show once a load has finished, so it hits the same cache entry. */
  fun imageRequest(url: String, width: Int, height: Int): ImageRequest? {
    val uri = Uri.parse(url) ?: return null
    return ImageRequestBuilder.newBuilderWithSource(uri)
      .setResizeOptions(ResizeOptions.forDimensions(width.coerceAtLeast(1), height.coerceAtLeast(1)))
      .build()
  }

  /**
   * Fetches [url] decoded within [width] by [height] pixels and reports its size.
   *
   * The bound is the column, not the screen: a 4,000-pixel photo drawn in a
   * 400-pixel column does not need four thousand pixels of bitmap. That is
   * `ResizeOptions` rather than a resize afterwards, so the decode itself is bounded.
   *
   * [onAnswer] runs on the main thread exactly once, unless the ticket is cancelled
   * first. An SVG, which neither platform's pipeline decodes, is a failure.
   */
  fun load(url: String, width: Int, height: Int, onAnswer: (Answer?) -> Unit): Ticket? {
    val image = imageRequest(url, width, height) ?: run {
      main.post { onAnswer(null) }
      return null
    }
    val key = "$url@${width}x$height"
    val ticket = Ticket(key)
    val existing = live[key]
    if (existing != null) {
      existing.waiting[ticket] = onAnswer
      return ticket
    }
    val request = Request(key, image)
    request.waiting[ticket] = onAnswer
    live[key] = request
    queued.addLast(key)
    drain()
    return ticket
  }

  /** Takes back one view's interest. The request itself stops only when nobody wants it. */
  fun cancel(ticket: Ticket?) {
    val current = ticket ?: return
    val request = live[current.key] ?: return
    request.waiting.remove(current)
    if (request.waiting.isNotEmpty()) {
      return
    }
    live.remove(current.key)
    queued.remove(current.key)
    request.source?.let { source ->
      request.source = null
      started--
      source.close()
    }
    drain()
  }

  private fun drain() {
    while (started < MAX_IN_FLIGHT) {
      val key = queued.removeFirstOrNull() ?: return
      val request = live[key] ?: continue
      // `getImagePipeline` throws when Fresco was never initialized, which cannot
      // happen in a running app but can in a test harness or an early attach.
      val pipeline = try {
        Fresco.getImagePipeline()
      } catch (error: IllegalStateException) {
        finish(request, null)
        continue
      }
      started++
      // No caller context: the cache key is then the URL and the size alone, which
      // is what lets two views showing one picture share one decode.
      val source = pipeline.fetchDecodedImage(request.image, null)
      request.source = source
      source.subscribe(
        object : BaseDataSubscriber<CloseableReference<CloseableImage>>() {
          override fun onNewResultImpl(dataSource: DataSource<CloseableReference<CloseableImage>>) {
            if (!dataSource.isFinished) {
              return
            }
            val reference = dataSource.result
            val answer = reference?.get()?.let { image ->
              if (image.width > 0 && image.height > 0) Answer(image.width, image.height) else null
            }
            // The handle is the pipeline's; nothing here keeps the bitmap, because
            // the Drawee view asks the pipeline for it again from the memory cache.
            CloseableReference.closeSafely(reference)
            settle(request, answer)
          }

          override fun onFailureImpl(dataSource: DataSource<CloseableReference<CloseableImage>>) {
            settle(request, null)
          }
        },
        UiThreadImmediateExecutorService.getInstance(),
      )
    }
  }

  /** A started request finished. A request already cancelled has nobody to tell. */
  private fun settle(request: Request, answer: Answer?) {
    if (live[request.key] !== request || request.source == null) {
      return
    }
    request.source?.close()
    request.source = null
    started--
    finish(request, answer)
    drain()
  }

  private fun finish(request: Request, answer: Answer?) {
    live.remove(request.key)
    val callbacks = request.waiting.values.toList()
    request.waiting.clear()
    for (callback in callbacks) {
      callback(answer)
    }
  }
}
