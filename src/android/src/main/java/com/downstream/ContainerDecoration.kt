package com.downstream

/**
 * One level of block nesting around a block, as the engine reported it.
 *
 * Its own file, with no Android imports, because this is data rather than
 * drawing: the layout rules that read it — a collapsible block's body height, the
 * indent a container adds — are the ones `tests/android` runs without a device.
 * `DocumentSpans.kt` holds the parts that build the spans themselves.
 *
 * One nesting level's decoration: its indent, the bar or marker it draws in that
 * indent, and the two metrics the drawing reads off it.
 *
 * The bar's width and the marker gap are here rather than in the view because
 * they are the theme's, and a view that read its own would be a second place
 * the values could differ from the measurement's.
 */
internal class ContainerDecoration(
  val leading: Float,
  val width: Float,
  val barColor: Int?,
  val continuesPrevious: Boolean,
  val marker: String?,
  val barWidth: Float = 0f,
  val markerGap: Float = 0f,
  /**
   * The marker's own color, which a theme can set apart from the list's: a
   * bullet from a number, or a finished task from an unfinished one.
   */
  val markerColor: Int? = null,
  /**
   * The engine's `ContainerKind` for this level, so a reader of the list can tell
   * a quote from a `<details>` without re-reading the document. The drawing still
   * branches on it rather than on the decoration's own fields, because one level
   * is one container either way.
   */
  val kind: Int = 0,
  /**
   * The container's own id, which is what a reader's choice for a collapsible
   * block is keyed by. Zero for the levels that predate it, which have no choice.
   */
  val id: Int = 0,
  /**
   * `<details open>`: the writer's starting state. False is also the answer for
   * every other container kind, so this is only read where the kind is
   * `CONTAINER_DETAILS`.
   */
  val open: Boolean = false,
  /**
   * True on the container's own first leaf, which is where a list item's marker
   * and a `<details>`'s summary are drawn. Named here because it is two different
   * things depending on the kind, and both mean "this leaf draws the chrome".
   */
  val summary: Boolean = false,
)
