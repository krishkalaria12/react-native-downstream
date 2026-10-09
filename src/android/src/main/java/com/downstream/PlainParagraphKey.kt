package com.downstream

/** Only unstyled, uncontained paragraphs carry this key. */
internal data class PlainParagraphKey(
  val content: String,
  val appearance: List<Any>,
)
