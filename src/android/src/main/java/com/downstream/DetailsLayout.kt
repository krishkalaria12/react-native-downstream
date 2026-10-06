package com.downstream

/**
 * The Kotlin mirror of `src/cpp/downstream/DetailsLayout.h`, which is the one
 * definition of how a collapsible block lays out. The shadow node, the measure
 * path, and the view all read this, so a measured block and a drawn one cannot
 * disagree.
 *
 * Mirrored by `downstream::resolveDetails` and `downstream::visibleTextHeight`;
 * `testDetailsLayout` and `testVisibleTextHeight` in
 * `src/cpp/tests/test_export.cpp` are the table both sides have to agree on.
 */

/** How one collapsible block lays out. */
internal data class DetailsBlockLayout(
  /** True when the reader's answer and the writer's `open` agree. */
  val open: Boolean = false,
)

/**
 * The layout of the collapsible block a container describes.
 *
 * `writtenOpen` is the writer's `open` attribute, and the key is the container's
 * own id: already unique, already counted in source order, already stable across
 * append-only snapshots, and already on every container this side draws from.
 */
internal fun resolveDetails(choices: ReaderChoices, containerId: Int, writtenOpen: Boolean) =
  DetailsBlockLayout(
    // A recorded choice *flips* the writer's state rather than only being able to
    // open it, which is what lets a reader close a block the writer left open. With
    // no record the writer decides, so the default is the document the app sent.
    open = hasChoice(choices.set(ChoiceKind.DetailsOpen), containerId) != writtenOpen,
  )

/** `ContainerKind::Details`. */
internal const val CONTAINER_DETAILS = 4

/**
 * True when this block sits inside a `<details>` whose body is not shown.
 *
 * Any one of them is enough: a block inside two collapsible blocks, the outer of
 * which is closed, is not visible however open the inner one is. Mirrors
 * `insideHiddenDetails`.
 */
internal fun insideHiddenDetails(containers: List<ContainerDecoration>, choices: ReaderChoices): Boolean =
  containers.any { decoration ->
    decoration.kind == CONTAINER_DETAILS &&
      !resolveDetails(choices, decoration.id, decoration.open).open
  }

/**
 * True when this block is a `<details>`'s summary: the container's own first
 * leaf, which is where the chevron and the pressed state are drawn.
 *
 * The innermost frame with the marker, because the marker is consumed by the
 * first leaf of *its* container: a nested summary is inside the outer one, so
 * asking the outer frame would put two chevrons on the outer line.
 */
internal fun isDetailsSummary(containers: List<ContainerDecoration>): Boolean {
  for (container in containers) {
    if (container.kind != CONTAINER_DETAILS) continue
    if (container.summary) return true
  }
  return false
}

/**
 * A collapsed run of a text segment: the character range of a block whose body is
 * closed. A range rather than a block index because the platform lays the segment
 * out as one text layout and its *lines* are what is hidden.
 */
internal class HiddenRange(val start: Int, val end: Int)

/**
 * The height a text segment shows, from the whole layout's height and the top of
 * each of its lines. Mirrors `visibleTextHeight`.
 *
 * A line goes when its first character lies inside a collapsed range, and the
 * lines come from the whole layout rather than a second layout of the visible
 * text — so the visible text wraps exactly as it would if the body were open, and
 * opening a body is a pure height change.
 */
internal fun visibleTextHeight(
  totalHeight: Int,
  lineCount: Int,
  lineTop: (Int) -> Int,
  lineStart: (Int) -> Int,
  hidden: List<HiddenRange>,
): Int {
  if (hidden.isEmpty() || lineCount < 1) {
    return totalHeight
  }
  var removed = 0
  for (line in 0 until lineCount) {
    val at = lineStart(line)
    if (hidden.any { at >= it.start && at < it.end }) {
      removed += lineTop(line + 1) - lineTop(line)
    }
  }
  return totalHeight - removed
}