package com.downstream

/** Run with python3 src/android/tests/check_native_document.py after the example Android build. */
fun main() {
  fun document(starts: IntArray, layouts: DoubleArray = DoubleArray(starts.size * 9)): NativeDocument {
    val blocks = IntArray(starts.size * 16)
    for (index in starts.indices) {
      blocks[index * 16 + 4] = starts[index]
      blocks[index * 16 + 5] = starts.getOrElse(index + 1) { starts[index] + 5 } - 1
    }
    return NativeDocument("", blocks, IntArray(0), IntArray(0), emptyArray(), IntArray(0), IntArray(0), layouts)
  }

  check(document(IntArray(0)).blockOf(0) == 0)
  val one = document(intArrayOf(0))
  check(one.blockOf(-1) == 0)
  check(one.blockOf(0) == 0)
  check(one.blockOf(100) == 0)

  // Distinct faces make an incorrect run-to-block lookup observable.
  val layouts = doubleArrayOf(
    28.0, 700.0, 0.0, 0.0, 9.0, 12.0, 0.0, 1.0, 1.0,
    16.0, 400.0, 1.0, 0.0, 9.0, 12.0, 0.0, 0.0, 0.0,
    14.0, 500.0, -1.0, 0.0, 9.0, 12.0, 0.0, 0.0, 2.0,
    24.0,
  )
  val styled = document(intArrayOf(0, 8, 14), layouts)
  val cases = listOf(-1 to 0, 0 to 0, 6 to 0, 7 to 0, 8 to 1, 13 to 1, 14 to 2, 18 to 2, 100 to 2)
  for ((offset, block) in cases) {
    check(styled.blockOf(offset) == block) { "offset $offset belongs to block $block" }
  }
  val paragraph = styled.blockOf(10)
  check(styled.blockFontWeight(paragraph) == 400f)
  check(styled.blockFontFamily(paragraph) == 1)
  check(!styled.blockItalic(paragraph))
  val code = styled.blockOf(15)
  check(styled.blockFontWeight(code) == 500f)
  check(styled.isCodeElement(code))
  check(styled.previousBottomMargin == 24f)
  check(document(intArrayOf(0)).previousBottomMargin == 0f)
  check(document(IntArray(0), doubleArrayOf(12.0)).previousBottomMargin == 12f)

  // Every offset agrees with a simple reverse scan across a larger table.
  val starts = IntArray(4096) { it * 7 }
  val many = document(starts)
  for (offset in -1..(starts.last() + 7)) {
    val expected = starts.indexOfLast { it <= offset }.coerceAtLeast(0)
    check(many.blockOf(offset) == expected)
  }
  println("NativeDocument regression checks passed")
}
