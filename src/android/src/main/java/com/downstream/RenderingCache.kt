package com.downstream

/**
 * Renderings have a count cap and an approximate ownership budget. A stream's
 * changing tail must not retain as many large historical versions as short
 * messages. Values evicted here remain valid for views holding them.
 */
internal class RenderingCache<K, V>(
  private val maxEntries: Int,
  private val maxCost: Long,
  private val costOf: (V) -> Long,
) {
  private class Entry<V>(val value: V, val cost: Long)
  private val entries = LinkedHashMap<K, Entry<V>>(16, 0.75f, true)
  private var cost = 0L
  private var latestOversizedKey: K? = null
  private var latestOversized: V? = null

  init {
    require(maxEntries > 0 && maxCost > 0)
  }

  @Synchronized
  fun get(key: K): V? =
    if (latestOversized != null && latestOversizedKey == key) latestOversized else entries[key]?.value

  /** Drops [key]'s value. A view still holding it keeps a valid value. */
  @Synchronized
  fun remove(key: K) {
    if (latestOversized != null && latestOversizedKey == key) {
      latestOversizedKey = null
      latestOversized = null
    }
    entries.remove(key)?.let { cost -= it.cost }
  }

  /** Drops every value whose key matches [predicate]. */
  @Synchronized
  fun removeAll(predicate: (K) -> Boolean) {
    val oversized = latestOversizedKey
    if (latestOversized != null && oversized != null && predicate(oversized)) {
      latestOversizedKey = null
      latestOversized = null
    }
    val iterator = entries.entries.iterator()
    while (iterator.hasNext()) {
      val entry = iterator.next()
      if (predicate(entry.key)) {
        cost -= entry.value.cost
        iterator.remove()
      }
    }
  }

  @Synchronized
  fun put(key: K, value: V) {
    val valueCost = costOf(value)
    require(valueCost >= 0)
    entries.remove(key)?.let { cost -= it.cost }
    latestOversizedKey = null
    latestOversized = null
    if (valueCost > maxCost) {
      // Keep the newest oversized preparation reusable until the next insertion.
      // This one-slot exception cannot accumulate historical oversized tails.
      // Ordinary messages keep their own byte budget.
      if (entries.size >= maxEntries) {
        val oldest = entries.entries.iterator()
        cost -= oldest.next().value.cost
        oldest.remove()
      }
      latestOversizedKey = key
      latestOversized = value
      return
    }
    // Evict before adding so the running cost cannot overflow on insertion.
    val iterator = entries.entries.iterator()
    while (entries.size >= maxEntries || cost > maxCost - valueCost) {
      val oldest = iterator.next()
      cost -= oldest.value.cost
      iterator.remove()
    }
    entries[key] = Entry(value, valueCost)
    cost += valueCost
  }
}
