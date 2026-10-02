package com.downstream

import kotlin.system.measureNanoTime

// Host microbenchmarks only: no Android Layout, text shaping, or drawing.
// Each query receives the UTF-16 offset of the first visible visual line.
private var consumed = 0L

private fun oldLineForOffset(starts: IntArray, offset: Int): Int {
  var index = 0
  while (index + 1 < starts.size && starts[index + 1] <= offset) index++
  return index
}

private fun oldLineStarts(text: CharSequence): IntArray {
  val starts = ArrayList<Int>(minOf(text.length / 24 + 1, 4096))
  starts.add(0)
  for (index in text.indices) {
    if (text[index] == '\n') starts.add(index + 1)
  }
  return starts.toIntArray()
}

private fun compare(name: String, before: () -> Long, after: () -> Long) {
  check(before() == after()) { "$name changed output" }
  repeat(50) { consumed = before(); consumed = after() }
  val beforeMs = ArrayList<Double>()
  val afterMs = ArrayList<Double>()
  repeat(7) { sample ->
    fun old() { beforeMs.add(measureNanoTime { consumed = before() } / 1e6) }
    fun new() { afterMs.add(measureNanoTime { consumed = after() } / 1e6) }
    if (sample % 2 == 0) { old(); new() } else { new(); old() }
  }
  println("{\"name\":\"$name\",\"before_ms\":$beforeMs,\"after_ms\":$afterMs,\"checksum\":$consumed}")
}

fun main() {
  for (lines in intArrayOf(2000, 20000, 100000)) {
    val starts = IntArray(lines) { it * 24 }
    val offsets = IntArray(5000) { ((it * 7919L) % lines).toInt() * 24 + 7 }
    for (offset in offsets) check(oldLineForOffset(starts, offset) == codeLineForOffset(starts, offset))
    compare("visible_line_lookup_$lines",
      { offsets.sumOf { oldLineForOffset(starts, it).toLong() } },
      { offsets.sumOf { codeLineForOffset(starts, it).toLong() } })
  }
  for ((name, text) in listOf(
    "line_index_20000_code" to "const value = compute(user.id, 42); // next value\n".repeat(20000),
    "line_index_100000_blank" to "\n".repeat(100000),
  )) {
    check(oldLineStarts(text).contentEquals(codeLineStarts(text)))
    compare(name,
      { var sum = 0L; repeat(20) { sum += oldLineStarts(text).sumOf { it.toLong() } }; sum },
      { var sum = 0L; repeat(20) { sum += codeLineStarts(text).sumOf { it.toLong() } }; sum })
  }
}
