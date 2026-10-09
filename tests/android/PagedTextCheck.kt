package com.downstream

fun main() {
  val source = "x".repeat(1023) + "🙂 café مرحبا\n" + "tail".repeat(800)
  val pages = source.chunked(PagedText.PAGE_UNITS).toTypedArray()
  val text = PagedText(pages)
  pages[0] = "mutated"
  check(text.length == source.length)
  check(source.indices.all { text[it] == source[it] })
  for (start in listOf(0, 1022, 1023, 1024, 2048, source.length)) {
    val end = minOf(start + 1030, source.length)
    val chars = CharArray(end - start + 2) { '!' }
    text.getChars(start, end, chars, 1)
    check(String(chars, 1, end - start) == source.substring(start, end))
    check(chars.first() == '!' && chars.last() == '!')
    val slice = text.subSequence(start, end)
    check(slice.toString() == source.substring(start, end))
    check(slice.subSequence(0, slice.length).toString() == slice.toString())
    if (slice.isNotEmpty()) check(slice[0] == source[start])
    val sliced = CharArray(slice.length)
    (slice as android.text.GetChars).getChars(0, slice.length, sliced, 0)
    check(String(sliced) == source.substring(start, end))
  }
  check(text.toString() == source && text.toString() === text.toString())
  check(runCatching { PagedText(emptyArray()) }.isFailure)
  check(runCatching { PagedText(arrayOf("short", "last")) }.isFailure)
  check(runCatching { text[-1] }.exceptionOrNull() is IndexOutOfBoundsException)
  check(runCatching { text[text.length] }.exceptionOrNull() is IndexOutOfBoundsException)
  check(runCatching { text.subSequence(1, 0) }.isFailure)
  check(runCatching { text.getChars(0, 1, CharArray(0), 0) }.isFailure)
  check(runCatching { text.getChars(0, 1, CharArray(3), Int.MAX_VALUE) }.isFailure)
  check(runCatching { text.getChars(-1, 1, CharArray(3), 0) }.isFailure)
  println("paged UTF-16 GetChars: range copies, surrogate seams, slice ownership and bounds passed")
}
