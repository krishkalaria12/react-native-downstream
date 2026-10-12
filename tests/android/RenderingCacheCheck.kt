package com.downstream

import java.util.concurrent.atomic.AtomicReference

private class Rendering(val text: String, val cost: Long)

private fun checkTwoMiBBudget() {
  val mib = 1024L * 1024
  val budget = 2 * mib
  val cache = RenderingCache<Int, Rendering>(128, budget) { it.cost }
  val a = Rendering("older visible message", mib)
  val b = Rendering("evicted visible message", mib)
  val c = Rendering("latest message", mib)
  cache.put(1, a)
  cache.put(2, b)
  check(cache.get(1) === a && cache.get(2) === b) { "exact 2 MiB ordinary budget was not retained" }
  cache.get(1)
  cache.put(3, c)
  check(cache.get(2) == null && cache.get(1) === a && cache.get(3) === c) {
    "2 MiB ordinary budget did not evict the least recent rendering"
  }
  check(b.text == "evicted visible message") { "2 MiB eviction invalidated a held rendering" }

  val oversized = Rendering("visible oversized message", 3 * mib)
  cache.put(4, oversized)
  check(cache.get(4) === oversized) { "newest preparation above 2 MiB was not retained" }
  check(cache.get(1) === a && cache.get(3) === c) { "oversized slot displaced ordinary 2 MiB contents" }
  val replacement = Rendering("newest oversized message", 4 * mib)
  cache.put(5, replacement)
  check(cache.get(4) == null && cache.get(5) === replacement) { "2 MiB cache retained historical oversized preparations" }
  check(oversized.text == "visible oversized message") { "oversized replacement invalidated a held rendering" }
  val small = Rendering("small new message", 1024)
  cache.put(6, small)
  check(cache.get(5) == null && cache.get(6) === small) { "ordinary insertion did not retire the 2 MiB oversized slot" }
  check(cache.get(1) == null && cache.get(3) === c) { "ordinary insertion did not enforce the 2 MiB budget after an oversized value" }
  check(a.text == "older visible message" && replacement.text == "newest oversized message") {
    "retiring cache ownership invalidated a held ordinary or oversized rendering"
  }

  val concurrent = RenderingCache<Int, Rendering>(128, budget) { it.cost }
  val cost = 32L * 1024
  val failure = AtomicReference<Throwable?>()
  val threads = List(4) { worker ->
    Thread {
      try {
        repeat(256) { iteration ->
          val key = worker * 256 + iteration
          concurrent.put(key, Rendering("$key", cost))
          concurrent.get(key)
        }
      } catch (error: Throwable) {
        failure.compareAndSet(null, error)
      }
    }.apply { start() }
  }
  threads.forEach { it.join() }
  check(failure.get() == null) { "concurrent 2 MiB cache access failed: ${failure.get()}" }
  val survivors = (0 until 1024).mapNotNull { concurrent.get(it) }
  check(survivors.size == 64 && survivors.sumOf { it.cost } == budget) {
    "concurrent ordinary writes exceeded or underfilled the 2 MiB ownership budget"
  }
  println("2 MiB rendering cache: ordinary LRU, held eviction, newest oversized replacement/retirement and concurrent ownership budget passed")
}

fun main() {
  checkTwoMiBBudget()
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

  // A view that moved to a newer snapshot retires every variant of the old one,
  // including the oversized slot, and frees their budget for other messages.
  val retiring = RenderingCache<String, Rendering>(128, 100) { it.cost }
  val mounted = Rendering("mounted old snapshot", 40)
  retiring.put("7:1.0:false", mounted)
  retiring.put("7:1.0:true", Rendering("other variant", 40))
  retiring.put("70:1.0:false", Rendering("another document", 20))
  retiring.put("8:1.0:false", Rendering("oversized newer snapshot", 500))
  retiring.removeAll { it.startsWith("7:") }
  check(retiring.get("7:1.0:false") == null && retiring.get("7:1.0:true") == null) { "retired snapshot stayed cached" }
  check(retiring.get("70:1.0:false")?.text == "another document") { "retiring a snapshot dropped another document" }
  check(retiring.get("8:1.0:false")?.text == "oversized newer snapshot") { "retiring dropped the newest oversized slot" }
  check(mounted.text == "mounted old snapshot") { "retiring invalidated a mounted rendering" }
  retiring.removeAll { it.startsWith("8:") }
  check(retiring.get("8:1.0:false") == null) { "retiring kept an oversized snapshot" }
  repeat(4) { retiring.put("fresh:$it", Rendering("fresh", 20)) }
  check((0 until 4).all { retiring.get("fresh:$it") != null }) { "retired entries still counted against the budget" }
  retiring.remove("fresh:0")
  retiring.put("fresh:4", Rendering("fresh", 40))
  check(retiring.get("fresh:0") == null && (1..4).all { retiring.get("fresh:$it") != null }) {
    "removing an entry did not free its budget"
  }

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
