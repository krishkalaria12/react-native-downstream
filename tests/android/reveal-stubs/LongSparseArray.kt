package android.util

class LongSparseArray<E> {
  private var keys = LongArray(8)
  private var values = arrayOfNulls<Any?>(8)
  private var length = 0

  fun get(key: Long): E? {
    val index = keys.binarySearch(key, 0, length)
    return if (index >= 0) values[index] as E else null
  }

  fun put(key: Long, value: E) {
    var index = keys.binarySearch(key, 0, length)
    if (index >= 0) {
      values[index] = value
      return
    }
    index = index.inv()
    ensure(length + 1)
    if (index < length) {
      keys.copyInto(keys, index + 1, index, length)
      values.copyInto(values, index + 1, index, length)
    }
    keys[index] = key
    values[index] = value
    length++
  }

  fun append(key: Long, value: E) {
    if (length > 0 && key <= keys[length - 1]) {
      put(key, value)
      return
    }
    ensure(length + 1)
    keys[length] = key
    values[length++] = value
  }

  fun clear() {
    java.util.Arrays.fill(values, 0, length, null)
    length = 0
  }

  fun size(): Int = length
  fun keyAt(index: Int): Long = keys[index]

  private fun ensure(wanted: Int) {
    if (wanted <= keys.size) return
    val grown = keys.copyOf(keys.size * 2)
    keys = grown
    values = values.copyOf(grown.size)
  }
}
