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

/**
 * The glyph keys the `icons` prop resolves, in the order the native views read
 * them. Mirrors `CONTROL_ICON_KEYS` in `src/icons.ts`, and append-only like it: the
 * array travels positionally, so a key inserted in the middle would give every
 * later button a different drawable on a release built against the older list.
 */
internal val ICON_KEYS = listOf(
  "copy", "check", "download", "maximize", "close",
  "wrap", "expand", "collapse", "tableFullscreen", "detailsChevron",
)

/**
 * The drawable each default key stands for.
 *
 * A table rather than a name compared at each call site, because the default is
 * the one thing a button cannot do without and a `when` in five places is five
 * places to forget a key.
 */
private val DEFAULT_ICONS = mapOf(
  "copy" to R.drawable.ds_ic_copy,
  "check" to R.drawable.ds_ic_check,
  "download" to R.drawable.ds_ic_download,
  "maximize" to R.drawable.ds_ic_fullscreen,
  "close" to R.drawable.ds_ic_check,
  "wrap" to R.drawable.ds_ic_wrap,
  "expand" to R.drawable.ds_ic_expand,
  "collapse" to R.drawable.ds_ic_collapse,
  "tableFullscreen" to R.drawable.ds_ic_fullscreen,
  // The disclosure is the expand arrow turned: the same vector the code card's
  // expand button draws, which is why there is no separate asset for it.
  "detailsChevron" to R.drawable.ds_ic_expand,
)

/** The resolved glyph names, in [ICON_KEYS] order. An empty string is a removed key. */
private var controlIcons: List<String> = emptyList()

/**
 * The glyph for one key, or the platform's default for an entry the app did not
 * name.
 *
 * A name with a dot in it is a drawable the app added, resolved at run time from
 * its own resources: an app that wants a different shape for one button names an
 * asset rather than asking this library to ship every shape it might want.
 */
internal fun controlDrawable(key: String, fallback: Int): Int {
  val index = ICON_KEYS.indexOf(key)
  val named = if (index >= 0 && index < controlIcons.size) controlIcons[index] else ""
  if (named.isEmpty()) {
    return DEFAULT_ICONS[key] ?: fallback
  }
  if (!named.contains('.')) {
    return fallback
  }
  return resolveDrawable(named) ?: DEFAULT_ICONS[key] ?: fallback
}

/** Sets the resolved names. Called from the view manager when the prop changes. */
internal fun setControlIcons(icons: List<String>) {
  controlIcons = icons
}

/**
 * A drawable by name, looked up in this library's resources.
 *
 * Only this package's own resources, because a glyph is a shape rather than
 * content and a name from a message is not a path this library should resolve.
 */
private fun resolveDrawable(name: String): Int? = try {
  val id = com.downstream.R.drawable::class.java.getField(name).getInt(null)
  if (id != 0) id else null
} catch (_: java.lang.Exception) {
  null
}

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

  /**
   * A press on a task list item's checkbox. The document does not change: the app
   * owns the source, which is the only thing that can flip the marker.
   */
  fun onToggleTask(containerId: Int, checked: Boolean)
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

/**
 * A hold recogniser: one link, one deadline, cancelled on lift or drift.
 *
 * A small class rather than a handler callback because the deadline has to be
 * armed on the down and disarmed on anything else, and a `Handler.postDelayed`
 * with no owner is how a hold outlives the touch that started it.
 */
internal class LongPress(
  private val context: android.content.Context,
  private val link: Int,
  private val fire: (Int) -> Unit,
) {
  private var handler: android.os.Handler? = null
  private var startX = 0f
  private var startY = 0f
  private var armed = false

  fun schedule(x: Float, y: Float) {
    cancel()
    startX = x
    startY = y
    armed = true
    val queue = android.os.Handler(android.os.Looper.getMainLooper())
    handler = queue
    queue.postDelayed(
      {
        armed = false
        fire(link)
      },
      android.view.ViewConfiguration.getLongPressTimeout().toLong(),
    )
  }

  fun moved(x: Float, y: Float) {
    if (!armed) return
    // A finger that drifted past the touch slop is scrolling or selecting, not
    // holding: the hold is cancelled rather than fired late.
    val slop = android.view.ViewConfiguration.get(context).scaledTouchSlop
    if (kotlin.math.abs(x - startX) > slop || kotlin.math.abs(y - startY) > slop) {
      cancel()
    }
  }

  fun cancel() {
    armed = false
    handler?.removeCallbacksAndMessages(null)
    handler = null
  }
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
