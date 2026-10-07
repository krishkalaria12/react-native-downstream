package com.downstream

/** Only cells without inline spans enter this cache; header spans are immutable. */
internal data class TableCellKey(
  val text: String,
  val themeId: Long,
  val scale: Float,
  val density: Float,
  val night: Boolean,
  val quoted: Boolean,
  val header: Boolean,
  val alignment: Int,
  val locales: String,
)
