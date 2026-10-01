package com.downstream

import android.content.res.Configuration
import android.graphics.Typeface

/**
 * One theme's resolved values, read once per theme id.
 *
 * The engine resolves the slots and the shared layout rules in C++, and this is
 * the Kotlin view of that answer: the renderer asks for a metric, a color or a
 * family by slot name and gets the same number both platforms get. Nothing here
 * decides anything a theme can decide, because a rule written twice in two
 * languages is a rule that will differ in one of them.
 *
 * Cached by id, since a theme is interned by content and every view on one theme
 * wants the same table.
 */
internal class NativeTheme private constructor(
  /** One entry per slot id, indexed by id. Meaningless for a color slot. */
  private val numbers: DoubleArray,
  /** The light half of every color, as ARGB. */
  private val light: IntArray,
  private val dark: IntArray,
  /** Which colors the theme set. A false entry draws the fallback instead. */
  private val set: BooleanArray,
  /** The families a font slot names, in the engine's order. */
  val families: List<String>,
) {
  /** A metric, weight or flag as a float, in points before font scaling. */
  fun number(slot: Int): Float = numbers[slot - 1].toFloat()

  fun flag(slot: Int): Boolean = numbers[slot - 1] != 0.0

  /**
   * Whether the theme named this color, as against leaving it at its default.
   *
   * A false entry draws the fallback instead, which is what makes a theme that
   * sets no zebra striping different from one that sets it transparent.
   */
  fun colorSet(slot: Int): Boolean = slot in set.indices && set[slot - 1]

  /**
   * Whether the theme named this slot, whatever kind it is.
   *
   * The named value and the default are the same number, and a renderer has to
   * be able to tell them: a `strong` whose weight slot is unset raises the
   * block's own weight so a bold heading still reads as bolder, while one that
   * named a weight means exactly that weight.
   */
  fun numberSet(slot: Int): Boolean = slot in set.indices && set[slot - 1]

  /**
   * The color for a slot in one appearance, or null when the theme left it
   * unset.
   *
   * Null is a real answer rather than a fallback: a theme that sets no zebra
   * striping is not the same as one that sets it transparent, and only the
   * first is "draw no fill at all".
   */
  fun color(slot: Int, night: Boolean): Int? {
    if (!colorSet(slot)) {
      return null
    }
    // Either half of a pair resolves to the same pair, so a caller that happens
    // to hold the dark id reads the same color it would read at the light one.
    val at = if (slot in light.indices) slot - 1 else return null
    return if (night) dark[at] else light[at]
  }

  /**
   * The family a font slot names, or the empty string for the system font.
   *
   * An unset family slot resolves to an empty name in the engine, so this needs
   * no default of its own.
   */
  fun family(slot: Int): String {
    val index = numbers[slot - 1].toInt()
    return if (index < 0 || index >= families.size) "" else families[index]
  }

  /**
   * The family at an index into the theme's list, or the empty string.
   *
   * The engine sends a block's family as an index rather than a name, so a
   * document's faces cost one number each instead of a string per block.
   */
  fun familyAt(index: Int): String =
    if (index < 0 || index >= families.size) "" else families[index]

  /**
   * The typeface for a family by name, at a weight.
   *
   * The block's face arrives as a resolved name rather than a slot, because the
   * engine already decided which slot a block's family is.
   */
  fun typeface(family: String, weight: Float, italic: Boolean, monospaced: Boolean = false): Typeface =
    resolveTypeface(family, weight, italic, monospaced)

  /**
   * The typeface for a font slot, from the family and weight the theme names.
   *
   * No size: Android scales a face at paint time from `TextPaint.textSize`, so a
   * face is shared across every size a theme uses and one lookup serves a whole
   * document. `Typeface.create` returns a cached face for the same family,
   * weight and slant, so this is a lookup rather than a construction.
   *
   * A weight the platform has no face for snaps to the nearest one it has, which
   * is what CSS font matching does and the reason a theme can name 250 rather
   * than only the hundred steps.
   */
  fun typeface(slot: Int, weight: Float, italic: Boolean, monospaced: Boolean = false): Typeface =
    resolveTypeface(family(slot), weight, italic, monospaced)

  private fun resolveTypeface(name: String, weight: Float, italic: Boolean, monospaced: Boolean): Typeface {
    // The family is turned into a face first because Android has no overload
    // taking a name with a weight: a weight and a slant are a modification of a
    // face, not of a name.
    val base = when {
      name.isNotEmpty() -> Typeface.create(name, Typeface.NORMAL)
      // An unset monospace family means the platform's own monospace.
      monospaced -> Typeface.MONOSPACE
      else -> Typeface.DEFAULT
    }
    if (weight == 400f && !italic) {
      // A regular face is the base itself, so it is returned as it is rather
      // than rebuilt: that is the common case, and rebuilding allocates.
      return base
    }
    return Typeface.create(base, weight.toInt(), italic)
  }

  /**
   * Inline code's size as a share of its block's.
   *
   * A ratio rather than a size because inline code keeps its proportions when a
   * theme changes the body size, unless the theme set an absolute one.
   */
  fun inlineCodeRatio(absolute: Float): Float =
    if (flag(ThemeSlots.SLOT_CODE_ABSOLUTE_SIZE)) {
      absolute / number(ThemeSlots.SLOT_PARAGRAPH_FONT_SIZE)
    } else {
      number(ThemeSlots.SLOT_CODE_SIZE_SCALE)
    }

  companion object {
    /**
     * The theme for an id, or the default theme when the pool does not hold it.
     *
     * A stale id is answered with the default rather than with nothing, so a
     * renderer that has lost its theme draws the platform's own look instead of
     * an unthemed document with every color missing.
     */
    private val cache = object : LinkedHashMap<Long, NativeTheme>(16, 0.75f, true) {
      // Bounded, because a screen previewing themes in a list should not keep
      // every value it ever showed alive.
      override fun removeEldestEntry(eldest: MutableMap.MutableEntry<Long, NativeTheme>) = size > 16
    }

    @Synchronized
    fun of(themeId: Long): NativeTheme = cache.getOrPut(themeId) {
      val parts = DownstreamNative.themeSnapshot(themeId)
      NativeTheme(
        numbers = parts[0] as DoubleArray,
        light = parts[1] as IntArray,
        dark = parts[2] as IntArray,
        set = parts[3] as BooleanArray,
        families = (parts[4] as Array<*>).map { it as String },
      )
    }

    /** Whether the device is currently in its dark appearance. */
    fun isNight(context: android.content.Context): Boolean =
      (context.resources.configuration.uiMode and Configuration.UI_MODE_NIGHT_MASK) ==
        Configuration.UI_MODE_NIGHT_YES
  }
}