package com.downstream

/**
 * The Kotlin mirror of `src/cpp/downstream/CodeLayout.h`, which is the one
 * definition of how a code card lays out. The shadow node, the measure path, and
 * the view all read this, so a measured card and a drawn card cannot disagree.
 *
 * Mirrored by `downstream::resolveCodeBlock`, `downstream::gutterDigits`, and
 * `downstream::codeCardHeight`; `testCodeBlockLayout`, `testGutterDigits`, and
 * `testCodeCardHeight` in `src/cpp/tests/test_export.cpp` are the table both
 * sides have to agree on.
 */
internal data class CodeLayoutProps(
  /** `codeBlockWrap`. */
  val wrap: Boolean = false,
  /** `lineNumbers`. */
  val lineNumbers: Boolean = true,
  /** `codeBlockMaxHeight` in pixels. 0 means no cap. */
  val maxHeight: Float = 400f,
)

/**
 * Which of a reader's choice sets a toggle acts on.
 *
 * The ordinal in each set counts something different, so a toggle has to say
 * which one it means: a bare ordinal would let a code wrap land on a table.
 */
internal enum class ChoiceKind { Wrap, CodeExpand, TableExpand, DetailsOpen }

/**
 * The reader's own choices, one set for every kind of collapsible block.
 *
 * One type rather than a pair of per-kind ones, because they are one reader's
 * state: a state update carries all of them, and four copies of the same question
 * is four places they can drift. The sets are separate because the ordinals are
 * separate counts — a code ordinal counts code blocks, a table ordinal counts
 * tables, and a details ordinal counts collapsible blocks.
 *
 * Ordinals are counted as the segment plan is built, so they are stable across
 * append-only snapshots and a choice survives streaming.
 */
internal class ReaderChoices(
  val wrapToggles: IntArray = IntArray(0),
  val codeExpanded: IntArray = IntArray(0),
  val tableExpanded: IntArray = IntArray(0),
  val detailsOpen: IntArray = IntArray(0),
) {
  override fun equals(other: Any?): Boolean =
    other is ReaderChoices && wrapToggles.contentEquals(other.wrapToggles) &&
      codeExpanded.contentEquals(other.codeExpanded) &&
      tableExpanded.contentEquals(other.tableExpanded) &&
      detailsOpen.contentEquals(other.detailsOpen)

  override fun hashCode(): Int {
    var hash = wrapToggles.contentHashCode()
    hash = 31 * hash + codeExpanded.contentHashCode()
    hash = 31 * hash + tableExpanded.contentHashCode()
    return 31 * hash + detailsOpen.contentHashCode()
  }

  /** The set one kind of choice lives in. */
  fun set(kind: ChoiceKind): IntArray = when (kind) {
    ChoiceKind.Wrap -> wrapToggles
    ChoiceKind.CodeExpand -> codeExpanded
    ChoiceKind.TableExpand -> tableExpanded
    ChoiceKind.DetailsOpen -> detailsOpen
  }

  /** The same choices with one set replaced wholesale, as a state map gives it. */
  fun replace(kind: ChoiceKind, values: IntArray): ReaderChoices = ReaderChoices(
    wrapToggles = if (kind == ChoiceKind.Wrap) values else wrapToggles,
    codeExpanded = if (kind == ChoiceKind.CodeExpand) values else codeExpanded,
    tableExpanded = if (kind == ChoiceKind.TableExpand) values else tableExpanded,
    detailsOpen = if (kind == ChoiceKind.DetailsOpen) values else detailsOpen,
  )

  /** The same choices with [ordinal] toggled in one set, kept ascending. */
  fun copyWith(kind: ChoiceKind, ordinal: Int): ReaderChoices = with(kind, ordinal, !set(kind).contains(ordinal))

  /** The same choices with [ordinal] added to or removed from one set. */
  fun with(kind: ChoiceKind, ordinal: Int, on: Boolean): ReaderChoices {
    val next = set(kind)
    if (next.contains(ordinal) == on) return this
    val values = if (on) insert(next, ordinal) else remove(next, ordinal)
    return ReaderChoices(
      wrapToggles = if (kind == ChoiceKind.Wrap) values else wrapToggles,
      codeExpanded = if (kind == ChoiceKind.CodeExpand) values else codeExpanded,
      tableExpanded = if (kind == ChoiceKind.TableExpand) values else tableExpanded,
      detailsOpen = if (kind == ChoiceKind.DetailsOpen) values else detailsOpen,
    )
  }

  private companion object {
    /**
     * Adds or removes [ordinal], keeping the set ascending, and returns the new
     * set. Built by hand rather than with a list, because the sets hold one entry
     * per block the reader has touched and are almost always empty.
     */
    fun toggled(values: IntArray, ordinal: Int): IntArray =
      if (values.contains(ordinal)) remove(values, ordinal) else insert(values, ordinal)

    fun remove(values: IntArray, ordinal: Int): IntArray {
      val index = values.binarySearch(ordinal)
      // The one copy that excludes it, so nothing else sees a stale set.
      return IntArray(values.size - 1) { source ->
        if (source < index) values[source] else values[source + 1]
      }
    }

    fun insert(values: IntArray, ordinal: Int): IntArray {
      // A miss is `-(insertion point) - 1`, so the new entry goes there.
      val insertAt = -values.binarySearch(ordinal) - 1
      val next = IntArray(values.size + 1)
      var source = 0
      for (target in next.indices) {
        next[target] = if (target == insertAt) ordinal else values[source++]
      }
      return next
    }
  }
}

/** Everything that decides how code lays out: the props and the reader's choices. */
internal class CodeLayout(
  val props: CodeLayoutProps = CodeLayoutProps(),
  val choices: ReaderChoices = ReaderChoices(),
) {
  override fun equals(other: Any?): Boolean =
    other is CodeLayout && props == other.props && choices == other.choices

  override fun hashCode(): Int = 31 * props.hashCode() + choices.hashCode()

  /** The same props with a new set of the reader's choices. */
  fun withChoices(choices: ReaderChoices): CodeLayout = CodeLayout(props, choices)
}

/** How one code block lays out. */
internal data class CodeBlockLayout(
  /** Laid out at the card's width rather than at its own length. */
  val wraps: Boolean,
  /** `props.lineNumbers && !meta.noLineNumbers`: the prop wins over the meta. */
  val lineNumbers: Boolean,
  /** The number the gutter draws beside the block's first line. */
  val firstNumber: Int,
  /** The cap this block actually has, in pixels. 0 means no cap. */
  val maxHeight: Float,
  /** The reader lifted this block's cap, so its button collapses it again. */
  val expanded: Boolean = false,
)

/**
 * The fence meta the engine parsed, as Android reads it. The engine does the
 * parsing, so no Kotlin code scans the info string a second way.
 */
internal data class FenceMeta(
  /** Sanitized by the engine: control and bidi characters removed, capped. */
  val title: String = "",
  val startLine: Int = 1,
  val noLineNumbers: Boolean = false,
)

/** The lines a capped block still shows, whatever the font scale. */
internal const val MIN_VISIBLE_LINES = 5f

/**
 * The space between the gutter and the code, in points. The gutter's width
 * includes it, and its numbers end where it begins. Mirrors `downstream::kGutterGap`.
 */
internal const val GUTTER_GAP = 12f

/** True when `ordinal` is in `values`, which callers keep ascending. */
internal fun hasChoice(values: IntArray, ordinal: Int): Boolean =
  values.binarySearch(ordinal) >= 0

/**
 * The layout of the code block at [ordinal], from the props, the reader's choices,
 * and that block's own fence meta.
 *
 * Mirrors `downstream::resolveCodeBlock` exactly.
 */
internal fun resolveCodeBlock(
  layout: CodeLayout,
  ordinal: Int,
  meta: FenceMeta,
): CodeBlockLayout = CodeBlockLayout(
  wraps = hasChoice(layout.choices.wrapToggles, ordinal) != layout.props.wrap,
  // The prop wins over the meta, so `lineNumbers={false}` hides numbers even
  // where a fence asked for them.
  lineNumbers = layout.props.lineNumbers && !meta.noLineNumbers,
  firstNumber = meta.startLine,
  // An expanded block has no cap, and so has an uncapped one: both reach the card
  // as the single value that means "no cap".
  maxHeight = if (isExpanded(layout, ordinal)) 0f else layout.props.maxHeight,
  expanded = isExpanded(layout, ordinal),
)

/** Expanded only while there is a cap to lift. Mirrors `resolveCodeBlock`. */
private fun isExpanded(layout: CodeLayout, ordinal: Int): Boolean =
  layout.props.maxHeight > 0f && hasChoice(layout.choices.set(ChoiceKind.CodeExpand), ordinal)

/**
 * The digits a gutter needs for the widest number it will draw.
 *
 * At least two, so the gutter does not widen when a block crosses line 10, and so
 * Streamdown's "line numbers over 100 wrap and start a new line" cannot happen.
 * An empty fence still draws one numbered line.
 *
 * Mirrors `downstream::gutterDigits`.
 */
internal fun gutterDigits(firstNumber: Int, lineCount: Int): Int {
  val lastNumber = if (lineCount <= 1) firstNumber else firstNumber + (lineCount - 1)
  var digits = 1
  var remaining = lastNumber
  while (remaining >= 10) {
    digits++
    remaining /= 10
  }
  return maxOf(digits, 2)
}

/**
 * The height of one code card: its header plus `min(body + 2 * padding, cap)`.
 *
 * The cap is on the body box with the card's padding inside it and the header
 * outside, which is the box Streamdown caps, and it is never below
 * [MIN_VISIBLE_LINES] lines so a capped block stays readable at an accessibility
 * text size. A card is never taller than its body.
 *
 * Mirrors `downstream::codeCardHeight`.
 */
internal fun codeCardHeight(
  block: CodeBlockLayout,
  headerHeight: Int,
  padding: Int,
  bodyHeight: Int,
  lineHeight: Int,
): Int {
  val body = bodyHeight + 2 * padding
  if (block.maxHeight <= 0f) return headerHeight + body
  val floor = (MIN_VISIBLE_LINES * lineHeight).toInt() + 2 * padding
  val cap = maxOf(block.maxHeight.toInt(), floor)
  return headerHeight + minOf(body, cap)
}

/**
 * The UTF-16 offset of each logical line in a code block's text.
 *
 * Found once per segment text, on the worker, so a gutter's draw pass can walk the
 * lines inside its dirty rect instead of searching the text for newlines every
 * frame. Always at least one entry.
 *
 * Mirrors `downstream::codeLineStarts`.
 */
internal fun codeLineStarts(text: CharSequence): IntArray {
  var starts = IntArray(minOf(text.length / 24 + 1, 4096))
  var count = 1
  for (index in text.indices) {
    if (text[index] == '\n') {
      if (count == starts.size) starts = starts.copyOf(starts.size * 2)
      starts[count++] = index + 1
    }
  }
  return if (count == starts.size) starts else starts.copyOf(count)
}

/**
 * The logical code line containing a visual line's starting UTF-16 offset.
 * Wrapped continuations stay on the same logical line. A trailing empty line
 * has its own entry at the text length, so an exact offset selects that line.
 * Uses an upper bound search so deep scrolling takes logarithmic work.
 */
internal fun codeLineForOffset(starts: IntArray, offset: Int): Int {
  var low = 0
  var high = starts.size
  while (low < high) {
    val middle = low + (high - low) / 2
    if (starts[middle] <= offset) low = middle + 1 else high = middle
  }
  return (low - 1).coerceAtLeast(0)
}
