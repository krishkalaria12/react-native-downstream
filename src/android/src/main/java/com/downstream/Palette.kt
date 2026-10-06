package com.downstream

/**
 * The colors one rendering draws with, for one theme in one appearance.
 *
 * Every entry is a slot read, with the value the renderer used before theming
 * existed as the fallback. That fallback is what makes an unthemed document
 * pixel-identical to the one this replaced, and it is why the platform's own
 * night colors sit here rather than in a theme: a theme that sets nothing should
 * still be right at two o'clock in the morning.
 */
internal class Palette(private val theme: NativeTheme, val night: Boolean) {
  /**
   * A slot's color, or one of [light] and [dark] when the theme left it unset.
   *
   * The pair is the platform's role color rather than one value, because an
   * unset slot has to follow the appearance the same way a set one does.
   */
  fun slot(id: Int, light: Int, dark: Int): Int = theme.color(id, night) ?: if (night) dark else light

  /**
   * A slot's color, or null when neither the theme nor the caller named one.
   *
   * Used where a theme's "no fill at all" has to be told from a fill that
   * happens to be transparent, such as a table's zebra striping.
   */
  fun slotOrNull(id: Int): Int? = theme.color(id, night)

  val text = slot(ThemeSlots.SLOT_PARAGRAPH_COLOR, 0xFF000000.toInt(), 0xFFFFFFFF.toInt())
  val secondary = slot(ThemeSlots.SLOT_BLOCKQUOTE_COLOR, 0x993C3C43.toInt(), 0x99EBEBF5.toInt())
  val link = slot(ThemeSlots.SLOT_LINK_COLOR, 0xFF007AFF.toInt(), 0xFF0A84FF.toInt())
  val codeBackground = slot(ThemeSlots.SLOT_CODE_BACKGROUND_COLOR, 0x1F767680, 0x3D767680)
  val codeBlockBackground =
    slot(ThemeSlots.SLOT_CODE_BLOCK_BACKGROUND_COLOR, 0x1F767680, 0x3D767680)
  val separator = slot(ThemeSlots.SLOT_TABLE_BORDER_COLOR, 0x4A3C3C43, 0x99545458.toInt())
  val rule = slot(ThemeSlots.SLOT_THEMATIC_BREAK_COLOR, 0x4A3C3C43, 0x99545458.toInt())
  val quoteBar = slot(ThemeSlots.SLOT_BLOCKQUOTE_BORDER_COLOR, 0xFFC7C7CC.toInt(), 0xFF48484A.toInt())
  val marker = slot(ThemeSlots.SLOT_LIST_MARKER_COLOR, 0x993C3C43.toInt(), 0x99EBEBF5.toInt())
  val codeText = slot(ThemeSlots.SLOT_CODE_BLOCK_COLOR, 0xFF000000.toInt(), 0xFFFFFFFF.toInt())
  val headerColor = slot(ThemeSlots.SLOT_CODE_BLOCK_HEADER_COLOR, secondary, secondary)
  val headerBackground =
    slotOrNull(ThemeSlots.SLOT_CODE_BLOCK_HEADER_BACKGROUND_COLOR)
  /**
   * The header's buttons, which a theme can tint apart from the row's text.
   *
   * A theme that named only the row's text gets buttons that match, because a
   * header and its controls read as one row.
   */
  val headerButtonColor =
    slotOrNull(ThemeSlots.SLOT_CODE_BLOCK_HEADER_BUTTON_COLOR) ?: headerColor
  /** The rule under a code card's header. Clear where a theme set none. */
  val headerBorderColor = slotOrNull(ThemeSlots.SLOT_CODE_BLOCK_HEADER_BORDER_COLOR)

  /** A table toolbar's buttons, which read as chrome rather than as content. */
  val toolbarButtonColor = slot(ThemeSlots.SLOT_TABLE_TOOLBAR_BUTTON_COLOR, secondary, secondary)

  /**
   * The selection highlight.
   *
   * The platform's own accent where a theme named none, which is what a reader
   * expects a selection to look like. `null` means "leave it alone", so a view
   * that never set one keeps whatever it inherited.
   */
  val selectionColor: Int? = theme.color(ThemeSlots.SLOT_SELECTION_COLOR, night)
  val tableHeaderBackground =
    slotOrNull(ThemeSlots.SLOT_TABLE_HEADER_BACKGROUND_COLOR) ?:
      slot(ThemeSlots.SLOT_CODE_BACKGROUND_COLOR, 0x1F767680, 0x3D767680)

  /**
   * One row's zebra fill, or null where the theme set none.
   *
   * `index` counts data rows from zero, so the header row is not counted and the
   * alternation starts on the first row of the body whichever that is.
   */
  fun rowStripe(index: Int): Int? =
    slotOrNull(
      if (index % 2 == 0) ThemeSlots.SLOT_TABLE_ROW_EVEN_BACKGROUND_COLOR
      else ThemeSlots.SLOT_TABLE_ROW_ODD_BACKGROUND_COLOR,
    )

  /** The syntax slot per engine token class, so a class picks its own color. */
  private val tokenSlots = intArrayOf(
    ThemeSlots.SLOT_SYNTAX_PLAIN,
    ThemeSlots.SLOT_SYNTAX_COMMENT,
    ThemeSlots.SLOT_SYNTAX_KEYWORD,
    ThemeSlots.SLOT_SYNTAX_TYPE,
    ThemeSlots.SLOT_SYNTAX_STRING,
    ThemeSlots.SLOT_SYNTAX_NUMBER,
    ThemeSlots.SLOT_SYNTAX_CONSTANT,
    ThemeSlots.SLOT_SYNTAX_REGEX,
    ThemeSlots.SLOT_SYNTAX_FUNCTION,
    ThemeSlots.SLOT_SYNTAX_PROPERTY,
    ThemeSlots.SLOT_SYNTAX_OPERATOR,
    ThemeSlots.SLOT_SYNTAX_PREPROCESSOR,
    ThemeSlots.SLOT_SYNTAX_TAG,
    ThemeSlots.SLOT_SYNTAX_ATTRIBUTE,
    ThemeSlots.SLOT_SYNTAX_INSERTED,
    ThemeSlots.SLOT_SYNTAX_DELETED,
    ThemeSlots.SLOT_SYNTAX_HEADING,
  )

  /**
   * One token class's color.
   *
   * An unset class draws the engine's palette, which the theme carries as the
   * unset value. Plain and Operator are the exception: unset, they take the code
   * block's own text color, as they do in GitHub's own themes.
   */
  fun token(tokenClass: Int): Int {
    val slot = tokenSlots.getOrNull(tokenClass) ?: return codeText
    theme.color(slot, night)?.let { return it }
    return if (tokenClass == NativeDocument.TOKEN_PLAIN || tokenClass == NativeDocument.TOKEN_OPERATOR) {
      codeText
    } else {
      theme.unsetColor(slot, night)
    }
  }

  /** GitHub alert colors; a type GitHub does not define gets a neutral bar. */
  fun admonition(type: String?): Int = when (type) {
    "note" -> slot(ThemeSlots.SLOT_ADMONITION_NOTE, 0xFF007AFF.toInt(), 0xFF0A84FF.toInt())
    "tip" -> slot(ThemeSlots.SLOT_ADMONITION_TIP, 0xFF34C759.toInt(), 0xFF30D158.toInt())
    "important" -> slot(ThemeSlots.SLOT_ADMONITION_IMPORTANT, 0xFFAF52DE.toInt(), 0xFFBF5AF2.toInt())
    "warning" -> slot(ThemeSlots.SLOT_ADMONITION_WARNING, 0xFFFF9500.toInt(), 0xFFFF9F0A.toInt())
    "caution" -> slot(ThemeSlots.SLOT_ADMONITION_CAUTION, 0xFFFF3B30.toInt(), 0xFFFF453A.toInt())
    else -> quoteBar
  }
}
