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
