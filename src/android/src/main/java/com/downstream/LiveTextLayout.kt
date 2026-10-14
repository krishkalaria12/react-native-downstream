package com.downstream

import android.text.Layout

/**
 * Text laid out ahead of its view, which the view draws without a `TextView`
 * layout of its own: a streaming paragraph, a growing tail of prose, or a code
 * body. Laid out on the measuring thread, so the UI thread only draws it.
 */
internal interface LiveTextLayout {
  /** The text at a layout width, or null where only the canonical `TextView` can lay it out. */
  fun frame(width: Int): LiveTextFrame?
}

/** Live text at one width: the layout a view draws and the height it measures. */
internal interface LiveTextFrame {
  val layout: Layout
  val height: Int
}
