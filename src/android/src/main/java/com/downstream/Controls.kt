package com.downstream

import android.content.Context
import android.text.Layout
import android.text.Spanned
import android.util.TypedValue
import android.widget.ImageButton
import android.widget.LinearLayout
import android.widget.ImageView

internal const val HIT_SLOP = 8f

/// The clipboard's label for a selection copied out of a text segment. A label
/// rather than a resource because it is never shown: the platform uses it to
/// describe the clip to another app, and there is nothing to localize in it.
internal const val COPIED_LABEL = "Downstream"

// Block kinds, matching `BlockKind` in the engine, so a control names the kind
// it acts on rather than a platform-local enum.
internal const val BLOCK_CODE = 2
internal const val BLOCK_TABLE_ROW = 5

// Buttons to build, as bits of `downstream::ControlBits`.
internal const val CONTROL_CODE_COPY = 1
internal const val CONTROL_CODE_DOWNLOAD = 2
internal const val CONTROL_TABLE_COPY = 4
internal const val CONTROL_TABLE_DOWNLOAD = 8
internal const val CONTROL_CODE_WRAP = 16
internal const val CONTROL_CODE_EXPAND = 32
internal const val CONTROL_IMAGE_DOWNLOAD = 64
/** Phase 2: a table's expand and full-screen buttons. */
internal const val CONTROL_TABLE_EXPAND = 128
internal const val CONTROL_TABLE_FULLSCREEN = 256

/** Phase 5: a collapsible block's toggle. */
internal const val CONTROL_DETAILS_TOGGLE = 512

// How long a copy button shows its check before going back, as in Streamdown.
internal const val COPIED_DURATION_MS = 2_000L

// Indexes into the label array `src/controls.ts` sends, in the same order.
internal val DEFAULT_LABELS = listOf(
  "Copy Code", "Download file", "Copied", "Wrap lines", "Don't wrap lines",
  // Appended, not inserted: `src/controls.ts` sends the labels positionally, so a
  // new key goes at the end rather than moving every later one.
  "Show all lines", "Show fewer lines",
  "Copy table", "Copy table as Markdown", "Copy table as CSV", "Copy table as TSV",
  "Download table", "Download table as CSV", "Download table as Markdown",
  "Markdown", "CSV", "TSV",
  "Image not available", "Download image", "Image", "Image blocked",
  // Phase 1: the link confirmation's four strings.
  "Open this link?", "Open", "Copy link", "Close",
  // Phase 2: the table's two buttons.
  "View fullscreen", "Exit fullscreen", "Show all rows", "Show fewer rows",
  // Phase 5: the details summary's chevron and its accessible name.
  "Show details", "Hide details",
)

internal const val LABEL_COPY_CODE = 0
internal const val LABEL_DOWNLOAD_FILE = 1
internal const val LABEL_COPIED = 2
internal const val LABEL_WRAP_CODE = 3
internal const val LABEL_UNWRAP_CODE = 4
internal const val LABEL_EXPAND_CODE = 5
internal const val LABEL_COLLAPSE_CODE = 6
internal const val LABEL_COPY_TABLE = 7
internal const val LABEL_DOWNLOAD_TABLE = 11
internal const val LABEL_FORMAT_MARKDOWN = 14
internal const val LABEL_FORMAT_CSV = 15
internal const val LABEL_FORMAT_TSV = 16
internal const val LABEL_IMAGE_NOT_AVAILABLE = 17
internal const val LABEL_IMAGE_DOWNLOAD = 18
internal const val LABEL_IMAGE_BLOCKED = 20
internal const val LABEL_VIEW_FULLSCREEN = 25
internal const val LABEL_EXPAND_TABLE = 27
internal const val LABEL_COLLAPSE_TABLE = 28
internal const val LABEL_EXPAND_DETAILS = 29
internal const val LABEL_COLLAPSE_DETAILS = 30

/**
 * What a control button did. The segment views hold no document, so they hand
 * the host a description and the host reads the block.
 */
internal interface ControlHost {
  fun onControl(
    kind: Int,
    blockIndex: Int,
    blockCount: Int,
    language: String,
    action: String,
    format: String,
  )

  fun onToggleWrap(ordinal: Int, language: String, wrapped: Boolean)
  fun onToggleExpand(ordinal: Int, language: String, expanded: Boolean)

  /**
   * A table's expand button. Its own method beside `onToggleExpand` because the
   * two are keyed by different ordinals: a code ordinal counts code blocks and a
   * table ordinal counts tables, so one method taking both would have to be told
   * which a caller meant.
   */
  fun onToggleTableExpand(ordinal: Int, expanded: Boolean)

  /** A table's full-screen button. The host owns the document and exports it. */
  fun onTableFullscreen(ordinal: Int)

  /**
   * A collapsible block's toggle. Its own method because the key is a container
   * id rather than an ordinal: it is already unique, already counted in source
   * order, and already on every frame a block carries.
   */
  fun onToggleDetails(containerId: Int, open: Boolean)
  fun onControlFailed(kind: Int, action: String, message: String)
}

// A control is square and fills the 32dp chrome row it sits in, with its glyph
// drawn at 18dp, about the size of the iOS symbol.
private const val CONTROL_BUTTON_DP = 32f
private const val CONTROL_GLYPH_DP = 18f

/** One code header or table toolbar button, sized in dp and tinted later. */
internal fun newControlButton(context: Context, drawable: Int): ImageButton = ImageButton(context).apply {
  val density = context.resources.displayMetrics.density
  val size = (CONTROL_BUTTON_DP * density).toInt()
  val inset = ((CONTROL_BUTTON_DP - CONTROL_GLYPH_DP) / 2 * density).toInt()
  layoutParams = LinearLayout.LayoutParams(size, size)
  setImageResource(drawable)
  scaleType = ImageView.ScaleType.FIT_CENTER
  setPadding(inset, inset, inset, inset)
  val ripple = TypedValue()
  context.theme.resolveAttribute(android.R.attr.selectableItemBackgroundBorderless, ripple, true)
  setBackgroundResource(ripple.resourceId)
}

/** Link index at a point in a laid-out text, or -1. */
internal fun linkAt(layout: Layout, x: Float, y: Float): Int {
  if (y < 0 || y > layout.height) {
    return -1
  }
  val line = layout.getLineForVertical(y.toInt())
  if (x < layout.getLineLeft(line) - HIT_SLOP || x > layout.getLineRight(line) + HIT_SLOP) {
    return -1
  }
  val offset = layout.getOffsetForHorizontal(line, x)
  val text = layout.text as? Spanned ?: return -1
  return text.getSpans(offset, offset, LinkSpan::class.java).firstOrNull()?.index ?: -1
}
