package com.downstream

import android.text.GetChars

/** Immutable UTF-16 pages exported from a native snapshot without joining its prefix. */
internal class PagedText(pages: Array<String>) : CharSequence, GetChars {
  private val pages = pages.copyOf()
  override val length: Int
  @Volatile private var flat: String? = null

  init {
    require(this.pages.isNotEmpty())
    for (index in 0 until this.pages.lastIndex) require(this.pages[index].length == PAGE_UNITS)
    require(this.pages.last().length in 1..PAGE_UNITS)
    length = Math.addExact(Math.multiplyExact(this.pages.size - 1, PAGE_UNITS), this.pages.last().length)
  }

  override fun get(index: Int): Char {
    requireIndex(index, length)
    return pages[index / PAGE_UNITS][index % PAGE_UNITS]
  }

  override fun getChars(start: Int, end: Int, dest: CharArray, destoff: Int) {
    requireRange(start, end, length)
    if (destoff < 0 || destoff > dest.size - (end - start)) {
      throw IndexOutOfBoundsException("destination offset=$destoff, length=${dest.size}")
    }
    var at = start
    var out = destoff
    while (at < end) {
      val page = pages[at / PAGE_UNITS]
      val offset = at % PAGE_UNITS
      val count = minOf(end - at, page.length - offset)
      page.toCharArray(dest, out, offset, offset + count)
      at += count
      out += count
    }
  }

  override fun subSequence(startIndex: Int, endIndex: Int): CharSequence {
    requireRange(startIndex, endIndex, length)
    return Slice(this, startIndex, endIndex)
  }

  /** Contiguous text is materialized only for consumers that explicitly request a String. */
  override fun toString(): String {
    flat?.let { return it }
    return synchronized(this) {
      flat ?: buildString(length) { pages.forEach { append(it) } }.also { flat = it }
    }
  }

  private class Slice(val source: PagedText, val start: Int, val end: Int) : CharSequence, GetChars {
    override val length: Int get() = end - start
    override fun get(index: Int): Char {
      requireIndex(index, length)
      return source[start + index]
    }
    override fun getChars(start: Int, end: Int, dest: CharArray, destoff: Int) {
      requireRange(start, end, length)
      source.getChars(this.start + start, this.start + end, dest, destoff)
    }
    override fun subSequence(startIndex: Int, endIndex: Int): CharSequence {
      requireRange(startIndex, endIndex, length)
      return Slice(source, start + startIndex, start + endIndex)
    }
    override fun toString(): String {
      val chars = CharArray(length)
      getChars(0, length, chars, 0)
      return String(chars)
    }
  }

  companion object {
    // Must match SnapshotText's immutable page size and the JNI export contract.
    const val PAGE_UNITS = 1024
    private fun requireIndex(index: Int, length: Int) {
      if (index < 0 || index >= length) throw IndexOutOfBoundsException("index=$index, length=$length")
    }
    private fun requireRange(start: Int, end: Int, length: Int) {
      if (start < 0 || end < start || end > length) {
        throw IndexOutOfBoundsException("range=$start..$end, length=$length")
      }
    }
  }
}
