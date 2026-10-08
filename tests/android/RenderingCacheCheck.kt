package com.downstream

import java.util.concurrent.atomic.AtomicReference

private class Rendering(val text: String, val cost: Long)

fun main() {
  val cache = RenderingCache<Long, Rendering>(128, 8L * 1024 * 1024) { it.cost }
  val heldTail = Rendering("still visible", 3L * 1024 * 1024)
  cache.put(1, heldTail)
  cache.put(2, Rendering("next tail", 3L * 1024 * 1024))
  val nextTail = Rendering("latest tail", 3L * 1024 * 1024)
  cache.put(3, nextTail)
  check(cache.get(1) == null) { "growing historical tails exceeded the byte budget" }
  check(cache.get(3) === nextTail) { "latest tail was not retained" }
  check(heldTail.text == "still visible") { "eviction invalidated a mounted rendering" }

  val small = Rendering("short message", 1024)
  cache.put(4, small)
  check(cache.get(4) === small) { "small-message hit was lost" }
  val oversized = Rendering("oversized tail", 9L * 1024 * 1024)
  cache.put(5, oversized)
  check(cache.get(5) === oversized) { "latest oversized preparation was discarded before mount" }
  check(cache.get(4) === small) { "oversized tail displaced a small message" }
  val nextOversized = Rendering("next oversized tail", 10L * 1024 * 1024)
  cache.put(6, nextOversized)
  check(cache.get(5) == null && cache.get(6) === nextOversized) { "historical oversized tails accumulated" }
  check(oversized.text == "oversized tail") { "oversized eviction invalidated a mounted rendering" }
  cache.put(7, small)
  check(cache.get(6) == null && cache.get(7) === small) { "next small insertion did not retire the oversized slot" }

  val limited = RenderingCache<Int, Rendering>(2, 100) { it.cost }
  val a = Rendering("a", 20)
  val b = Rendering("b", 20)
  val c = Rendering("c", 20)
  limited.put(1, a)
  limited.put(2, b)
  check(limited.get(1) === a)
  limited.put(3, c)
  check(limited.get(2) == null && limited.get(1) === a) { "entry cap did not evict the least recent hit" }
  limited.put(1, Rendering("replacement", 90))
  check(limited.get(3) == null && limited.get(1)?.text == "replacement") { "replacement cost was not updated" }

  val concurrent = RenderingCache<Int, Rendering>(128, 4096) { it.cost }
  val threadFailure = AtomicReference<Throwable?>()
  val threads = List(4) { worker ->
    Thread {
      try {
        repeat(1000) { iteration ->
          val key = worker * 1000 + iteration
          concurrent.put(key, Rendering("$key", 64))
          concurrent.get(key)
        }
      } catch (error: Throwable) {
        threadFailure.compareAndSet(null, error)
      }
    }.apply { start() }
  }
  threads.forEach { it.join() }
  check(threadFailure.get() == null) { "concurrent cache access failed: ${threadFailure.get()}" }
  val survivors = (0 until 4000).count { concurrent.get(it) != null }
  check(survivors == 64) { "concurrent puts violated the ownership budget: $survivors survivors" }
  println("rendering cache checks passed")
}
