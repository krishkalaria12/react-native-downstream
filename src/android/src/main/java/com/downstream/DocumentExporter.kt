package com.downstream

import android.content.Context
import android.content.ContextWrapper
import android.net.Uri
import android.os.Handler
import android.os.Looper
import android.webkit.MimeTypeMap
import androidx.activity.ComponentActivity
import androidx.activity.result.ActivityResultLauncher
import androidx.activity.result.ActivityResultRegistry
import androidx.activity.result.contract.ActivityResultContracts
import com.facebook.react.bridge.ReactContext
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicLong

/**
 * Saves an export through the system "save to" dialog, which is the mobile
 * equivalent of the download link Streamdown puts on a file.
 *
 * `ACTION_CREATE_DOCUMENT` needs no permission and no `FileProvider`, and the
 * returned URI is written through the content resolver, so nothing is exposed
 * to the rest of the system and no manifest entry is required. The write runs
 * off the main thread because a large table can take a while.
 *
 * The content lives only in memory, so a save whose activity is recreated
 * while the dialog is open is lost. That is reported rather than dropped.
 */
internal class DocumentExporter(private val context: Context) {
  private var released = false

  companion object {
    // All views share one writer. Recycling a view cannot leave another
    // executor behind or shut down a save belonging to a different view.
    private val writer = Executors.newSingleThreadExecutor()
    private val nextKey = AtomicLong()
    private val main = Handler(Looper.getMainLooper())
  }

  /**
   * Shows the save dialog for `data` under `fileName`. The completion runs on
   * the main thread with the URI the reader chose, or with a message when the
   * write failed. A cancelled dialog calls neither.
   */
  fun save(
    fileName: String,
    mimeType: String,
    data: ByteArray,
    completion: (Uri?, String?) -> Unit,
  ) {
    if (released) return
    val activity = activity()
    if (activity == null) {
      completion(null, "No activity to present the save dialog from.")
      return
    }
    val registry = activity.activityResultRegistry
    val key = "downstream-save-${nextKey.incrementAndGet()}"
    var launcher: ActivityResultLauncher<String>? = null
    launcher = registry.register(
      key,
      ActivityResultContracts.CreateDocument(mimeType),
    ) { uri: Uri? ->
      // The key is spent the moment a result arrives, whichever way it arrived.
      launcher?.unregister()
      if (uri == null) {
        // A cancel is neither a success nor a failure, so it reports nothing.
        return@register
      }
      writer.execute {
        val error = write(uri, data)
        // A picker already shown belongs to the reader even if its view was
        // recycled. Finish the file; the view suppresses its stale event.
        main.post { completion(if (error == null) uri else null, error) }
      }
    }
    launcher.launch(fileName)
  }

  private fun activity(): ComponentActivity? {
    var owner = context
    while (true) {
      if (owner is ComponentActivity) return owner
      if (owner is ReactContext) (owner.currentActivity as? ComponentActivity)?.let { return it }
      val base = (owner as? ContextWrapper)?.baseContext ?: return null
      if (base === owner) return null
      owner = base
    }
  }

  /** The MIME type a file name's extension asks for, or the caller's fallback. */
  fun mimeTypeFor(fileName: String, fallback: String): String {
    val dot = fileName.lastIndexOf('.')
    if (dot < 0 || dot == fileName.length - 1) return fallback
    val extension = fileName.substring(dot + 1)
    return MimeTypeMap.getSingleton().getMimeTypeFromExtension(extension) ?: fallback
  }

  private fun write(uri: Uri, data: ByteArray): String? {
    return try {
      val stream = context.contentResolver.openOutputStream(uri, "wt")
        ?: return "The chosen file could not be opened."
      stream.use {
        it.write(data)
        it.flush()
      }
      null
    } catch (error: Exception) {
      error.message ?: "The file could not be written."
    }
  }

  fun release() {
    released = true
  }
}
