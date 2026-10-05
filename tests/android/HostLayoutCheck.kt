package com.downstream

import kotlin.system.exitProcess

/**
 * The contract in `src/cpp/downstream/HostLayout.h`, run against the Kotlin
 * mirror.
 *
 * `HostLayout.kt` is the platform's copy of the engine's host layout: the shadow
 * node measures with the C++ one and the view draws with this one, so both have
 * to answer "is this block drawn by a React child, and how tall is it" the same
 * way. Running one table against both makes a divergence a failing test rather
 * than a fence whose card is 40 pixels taller on one platform.
 *
 * The C++ side is `test_fence_slots.cpp`'s `host_layout` table, and every case
 * here is that table in the same order.
 *
 * Compiled and run by `pnpm test:kotlin`, which needs a Kotlin compiler and so is
 * not part of `pnpm test:native`.
 */
private var failures = 0

private fun check(condition: Boolean, label: String) {
  if (!condition) {
    println("FAIL $label")
    failures++
  }
}

private fun checkEq(actual: Float, expected: Float, label: String) =
  check(actual == expected, "$label: expected $expected, actual $actual")

fun main() {
  // An empty layout hosts nothing: a view with no renderers draws every fence as
  // its code card, which is the case this feature must not change.
  val empty = HostLayout()
  check(empty.isEmpty(), "an empty host layout is empty")
  checkEq(empty.heightFor(0), 0f, "an empty layout has no height")
  check(!empty.hosts(0), "an empty layout hosts nothing")

  // One entry, sorted by ordinal, looked up by binary search.
  val one = HostLayout(listOf(HostEntry(ordinal = 3, height = 120f)))
  check(!one.isEmpty(), "one entry is not empty")
  checkEq(one.heightFor(3), 120f, "one entry answers its own ordinal")
  checkEq(one.heightFor(2), 0f, "an ordinal before the first entry has no height")
  checkEq(one.heightFor(4), 0f, "an ordinal after the last entry has no height")
  check(one.hosts(3), "one entry hosts its ordinal")
  check(!one.hosts(2), "one entry hosts nothing else")

  // Several entries, in the order the engine writes them.
  val many = HostLayout(
    listOf(
      HostEntry(ordinal = 0, height = 40f),
      HostEntry(ordinal = 2, height = 80f),
      HostEntry(ordinal = 9, height = 200f),
    ),
  )
  checkEq(many.heightFor(0), 40f, "the first entry answers")
  checkEq(many.heightFor(1), 0f, "a gap answers zero")
  checkEq(many.heightFor(2), 80f, "the middle entry answers")
  checkEq(many.heightFor(9), 200f, "the last entry answers")
  checkEq(many.heightFor(10), 0f, "past the last entry answers zero")
  check(!many.hosts(1), "a gap hosts nothing")

  // A zero height is a component that rendered nothing: it still owns its
  // fence, so the code card stays away rather than appearing behind a component
  // that chose to draw nothing.
  val blank = HostLayout(listOf(HostEntry(ordinal = 1, height = 0f)))
  checkEq(blank.heightFor(1), 0f, "a zero height is zero")
  check(blank.hosts(1), "a zero height still hosts its fence")

  if (failures > 0) {
    println("FAIL host layout: $failures check(s) failed")
    exitProcess(1)
  }
  println("ok host layout")
}
