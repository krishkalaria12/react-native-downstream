package com.downstream
fun main() {
 val count = 64
 val ranges = LongArray(count * 3) { when (it % 3) { 0 -> (it / 3).toLong(); 1 -> 0L; else -> 4L } }
 val starts = DoubleArray(count) { it * 16.0 }
 val timing = doubleArrayOf(1.0, 0.0, 150.0, 16.0, 1000.0, 0.25, 0.1, 0.25, 1.0, 1.0)
 val frame = RevealFrame(timing, 3f, false)
 val targets = RevealTargets()
 targets.build(ranges, IntArray(count) { it + 1 })
 for (now in listOf(0.0, 500.0, 1000.0, 1100.0, 1158.0)) {
   frame.build(ranges, starts, now)
   val activeBlocks = (0 until frame.size).map { frame.unitAt(it).block }.distinct().size
   println("nowMs=$now frameUnits=${frame.size} activeRoots=$activeBlocks invalidatedRoots=${targets.size}")
 }
}
