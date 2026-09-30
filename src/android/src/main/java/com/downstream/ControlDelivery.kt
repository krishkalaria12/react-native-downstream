package com.downstream

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.os.Build
import java.text.DecimalFormatSymbols
import java.util.Locale

/**
 * The clipboard and the save dialog, which are the two ways a control delivers
 * what it exported. Both are system APIs only: the clipboard needs no
 * permission, and a save dialog neither needs one nor a manifest entry.
 */
internal object ControlDelivery {

  /** A CSV separator the locale chooses: a comma unless this locale writes decimals with one. */
  fun resolveSeparator(configured: String): Char = when (configured) {
    ";" -> ';'
    "\t", "\\t" -> '\t'
    "auto" -> if (DecimalFormatSymbols.getInstance(Locale.getDefault()).decimalSeparator == ',') ';' else ','
    else -> ','
  }

  /**
   * Writes a code block's text. A pasteboard write never shows the paste
   * permission prompt, so nothing has to be asked for first.
   */
  fun copyText(context: Context, label: String, text: String): Boolean {
    val clipboard = context.getSystemService(Context.CLIPBOARD_SERVICE) as? ClipboardManager ?: return false
    return try {
      clipboard.setPrimaryClip(ClipData.newPlainText(label, text))
      true
    } catch (error: RuntimeException) {
      // A block can be larger than the transaction Binder will carry, and a
      // clip that does not fit throws rather than truncating. Reporting the
      // failure is better than silently copying half a function.
      false
    }
  }

  /** Writes a table as text, and as HTML so a paste lands as a table. */
  fun copyTable(context: Context, text: String, html: String): Boolean {
    val clipboard = context.getSystemService(Context.CLIPBOARD_SERVICE) as? ClipboardManager ?: return false
    return try {
      clipboard.setPrimaryClip(ClipData.newHtmlText("Table", text, html))
      true
    } catch (error: RuntimeException) {
      false
    }
  }

  /**
   * Announces a copy to TalkBack.
   *
   * Android 13 and later show their own clipboard confirmation, and the
   * platform guidance is not to add a second one, so the announcement stops
   * there. A check glyph in the button is not announced on its own, so below
   * that the change is spoken.
   */
  fun announceCopied(view: android.view.View, message: String) {
    if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU) {
      view.announceForAccessibility(message)
    }
  }
}
