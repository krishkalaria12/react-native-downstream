package com.downstream

import android.os.Build
import android.text.Layout
import android.text.SpannableStringBuilder
import android.text.Spanned
import android.text.StaticLayout
import android.text.TextPaint
import android.text.TextUtils
import android.text.style.AbsoluteSizeSpan
import android.text.style.BackgroundColorSpan
import android.text.style.CharacterStyle
import android.text.style.UpdateAppearance
import android.text.style.ForegroundColorSpan
import android.text.style.LeadingMarginSpan
import android.text.style.StrikethroughSpan
import android.text.style.UnderlineSpan
import android.text.style.RelativeSizeSpan
import android.text.style.StyleSpan
import android.text.style.TypefaceSpan
import androidx.annotation.RequiresApi
import kotlin.math.ceil

/**
 * The width an unwrapped code body lays out at: the largest size a view can
 * measure, so no line ever breaks. Lines are sized by their own extent.
 */
internal const val UNBOUNDED_CODE_WIDTH = (1 shl 30) - 1

/**
 * A span whose effect on layout and drawing is fully described by a value.
 *
 * A chunk of paragraphs laid out under one snapshot's spans is reused by the
 * next snapshot only when that snapshot's spans over the same range are equal, and
 * every snapshot builds new span objects. A span answers null when part of what
 * it draws is not in its value, which keeps its chunk from ever being reused.
 */
internal interface ReusableSpan {
  val reuseKey: Any?
}

/**
 * Growing text laid out in chunks of whole paragraphs, so a snapshot lays out
 * only its last chunk: an open code fence, or prose a reply is still adding to.
 *
 * Both are rebuilt on every snapshot, and laying out their whole text each time
 * made every delta cost as much as the text was long, on the measuring thread and
 * again in a `TextView` on the UI thread. Paragraphs are independent: `StaticLayout`
 * breaks, measures and draws each one on its own, so a chunk laid out on its own
 * matches the same paragraphs inside the whole text exactly. A code line is a
 * paragraph too. Every chunk but the last holds complete paragraphs, closed at
 * [CHUNK_LINES] of them or [CHUNK_CHARACTERS] of text, and is reused by a later
 * snapshot whose text and span values over the same range are equal.
 *
 * A chunk is laid out over a copy that begins one character before its first
 * line, on the newline that ends the previous chunk. A span that started before
 * the chunk still starts before its first line in the copy, so a span that treats
 * its first line specially, as a block's spacing does, sees the same first line it
 * would in the whole text.
 *
 * Built and read under this object's lock from any thread; drawn on the UI
 * thread. The source is never mutated after the segment that owns it is built.
 */
@RequiresApi(Build.VERSION_CODES.P)
internal class ChunkedTextLayout(
  private val source: Spanned,
  /** The offset of each paragraph, which the segment found once. */
  private val lineStarts: IntArray,
  paint: TextPaint,
  previous: ChunkedTextLayout? = null,
  /**
   * Whether a frame measures its widest line as it is laid out. A code card reads
   * that width when it binds on the UI thread, so it is measured on the thread
   * that lays the body out; prose never reads it.
   */
  private val extentAhead: Boolean = false,
) : LiveTextLayout {
  private val paint = TextPaint(paint)
  private val metrics = ParagraphMetrics(this.paint)

  /**
   * The previous snapshot's settled chunks at each width it laid out. Chunks only,
   * so a long stream never chains one snapshot's text to the next.
   */
  private val seeds: Map<Int, List<TextChunk>> =
    if (previous != null && previous.metrics == metrics) previous.settledChunks() else emptyMap()
  private val frames = LinkedHashMap<Int, ChunkedTextFrame?>()

  /** This body's settled chunks, or the ones it was seeded with before it laid out. */
  @Synchronized
  private fun settledChunks(): Map<Int, List<TextChunk>> {
    if (frames.isEmpty()) return seeds
    val chunks = HashMap<Int, List<TextChunk>>()
    for ((width, frame) in frames) if (frame != null) chunks[width] = frame.settled
    return chunks
  }

  /** The text at a layout width, or null where only the canonical `TextView` can draw it. */
  @Synchronized
  override fun frame(width: Int): ChunkedTextFrame? {
    if (frames.containsKey(width)) return frames[width]
    val result = build(width, seeds[width])
    // Read for its effect: the width is measured once, here, and kept.
    if (extentAhead) result?.contentWidth
    // Two widths: the one a card draws at and the one a measurement may ask
    // about while a rotation or a wrap toggle settles.
    if (frames.size == 2) frames.remove(frames.keys.first())
    frames[width] = result
    return result
  }

  /**
   * The first paragraph of each settled chunk, then the tail's. The last paragraph
   * can still change, so only the paragraphs before it settle, and a chunk settles
   * only once it is full: the boundaries depend on nothing but the settled text, so
   * the next snapshot finds the same ones.
   */
  private fun chunkStarts(): IntArray {
    val last = lineStarts.size - 1
    val starts = ArrayList<Int>()
    var first = 0
    for (line in 1..last) {
      if (line - first >= CHUNK_LINES || lineStarts[line] - lineStarts[first] >= CHUNK_CHARACTERS) {
        starts.add(first)
        first = line
      }
    }
    starts.add(first)
    return starts.toIntArray()
  }

  private fun build(width: Int, seed: List<TextChunk>?): ChunkedTextFrame? {
    val chunkLines = chunkStarts()
    val settledCount = chunkLines.size - 1
    val settled = ArrayList<TextChunk>(settledCount)
    var reused = 0
    var laidOut = 0
    for (index in 0 until settledCount) {
      val start = lineStarts[chunkLines[index]]
      // The chunk stops before the newline that ends its last paragraph, so its
      // own layout does not open the empty line a trailing newline would.
      val end = lineStarts[chunkLines[index + 1]] - 1
      val candidate = seed?.getOrNull(index)
      val chunk = if (candidate != null && candidate.matches(source, start, end)) {
        reused++
        candidate
      } else {
        laidOut += end - start
        buildChunk(start, end, width, settled = true) ?: return null
      }
      settled.add(chunk)
    }
    val tailStart = lineStarts[chunkLines[settledCount]]
    val tail = buildChunk(tailStart, source.length, width, settled = false) ?: return null
    val parts = ArrayList<LayoutPart>(settled.size + 1)
    var height = 0
    for (chunk in settled) {
      parts.add(LayoutPart(chunk.layout, chunk.base, chunk.layout.lineCount, chunk.recording))
      height += chunk.layout.height
    }
    // The tail records on its second draw: the next snapshot replaces it, so a
    // tail drawn once is drawn directly, but a fade redraws it in one pass per
    // alpha every frame, and replaying is what keeps those passes from shaping
    // the same lines again.
    parts.add(LayoutPart(tail.layout, tail.base, tail.layout.lineCount, tail.recording))
    height += tail.layout.height
    return ChunkedTextFrame(
      text = source,
      layout = CompositeLayout(source, paint, width, parts),
      height = height,
      chunks = settled + tail,
      settled = settled,
      reusedChunks = reused,
      laidOutCharacters = laidOut + source.length - tailStart,
    )
  }

  /** Lays out source[start, end), or null for an unwrapped line that reads right to left. */
  private fun buildChunk(start: Int, end: Int, width: Int, settled: Boolean): TextChunk? {
    val base = if (start == 0) 0 else start - 1
    val copy = SpannableStringBuilder(source, base, end)
    val spans = if (settled) SpanSignature.of(copy, 0, copy.length) else null
    // The canonical `TextView` shows precomputed text, and a layout over
    // precomputed text draws no underline or strike: the view's decoration pass
    // draws those. Over plain text a layout would draw them too, so the copy
    // drops them and draws exactly what the canonical layout does, without
    // keeping every word's shaped glyphs alive the way precomputed text does.
    if (copy.isNotEmpty()) copy.setSpan(NoNativeDecorations, 0, copy.length, Spanned.SPAN_INCLUSIVE_INCLUSIVE)
    val layout = RenderedDocument.buildLayout(copy, paint, width, Layout.Alignment.ALIGN_NORMAL, start - base, copy.length)
    // An unwrapped right-to-left line aligns to a layout edge no view can reach,
    // and a card aligns it to the body's own width. That is the canonical
    // `TextView`'s job, so such a body is not laid out here at all.
    if (width == UNBOUNDED_CODE_WIDTH &&
      (0 until layout.lineCount).any { layout.getParagraphDirection(it) == Layout.DIR_RIGHT_TO_LEFT }
    ) {
      return null
    }
    return TextChunk(
      text = copy,
      base = base,
      start = start,
      end = end,
      layout = layout,
      spans = spans,
      recording = PartRecording(deferred = !settled),
    )
  }

  companion object {
    /** Paragraphs per settled chunk at most: a code block's lines. */
    const val CHUNK_LINES = 16

    /** Text per settled chunk at most, so prose settles in chunks of a few paragraphs. */
    const val CHUNK_CHARACTERS = 1024
  }
}

/** Chunked text at one layout width. */
internal class ChunkedTextFrame(
  /** The whole text, which [layout]'s offsets index. */
  val text: Spanned,
  override val layout: Layout,
  override val height: Int,
  private val chunks: List<TextChunk>,
  internal val settled: List<TextChunk>,
  /** Settled chunks taken from the previous snapshot rather than laid out again. */
  val reusedChunks: Int,
  /** Characters this frame laid out itself. */
  val laidOutCharacters: Int,
) : LiveTextFrame {
  /**
   * The widest line, rounded up: what an unwrapped body scrolls. Only a code
   * card asks, so prose never measures its lines a second time.
   */
  val contentWidth: Int by lazy {
    val extent = chunks.maxOfOrNull { it.extent } ?: 0f
    ceil(extent.toDouble()).coerceIn(1.0, UNBOUNDED_CODE_WIDTH.toDouble()).toInt()
  }
}

/**
 * Clears what a layout would draw as underline or strike, applied last on a
 * chunk's own copy. Shared, because it holds nothing: every copy can carry it.
 */
private object NoNativeDecorations : CharacterStyle(), UpdateAppearance {
  override fun updateDrawState(paint: TextPaint) {
    paint.isUnderlineText = false
    paint.isStrikeThruText = false
    paint.underlineColor = 0
  }
}

/** Whole paragraphs of chunked text, laid out once. */
internal class TextChunk(
  /** source[base, end), the copy [layout] was laid out over. */
  val text: Spanned,
  /** The source offset of the copy's first character. */
  val base: Int,
  /** The source offset of the chunk's first line. */
  val start: Int,
  /** The source offset the chunk stops at: its last newline, or the source end. */
  val end: Int,
  val layout: StaticLayout,
  /** The spans the chunk was laid out under, or null when it is not reusable. */
  private val spans: SpanSignature?,
  val recording: PartRecording,
) {
  /**
   * The widest line's width, including trailing spaces and tabs as the canonical
   * measurement does. Measured only when a code card asks for its scroll width,
   * because measuring every line again costs a third of laying the chunk out.
   */
  val extent: Float by lazy {
    var widest = 0f
    for (line in 0 until layout.lineCount) widest = maxOf(widest, layout.getLineWidth(line))
    widest
  }

  /** True when [source] holds this chunk's lines, under equal spans, at the same place. */
  fun matches(source: Spanned, start: Int, end: Int): Boolean {
    if (start != this.start || end != this.end || end > source.length) return false
    val spans = spans ?: return false
    return TextUtils.regionMatches(source, base, text, 0, text.length) && spans.matches(source, base, end)
  }
}

/**
 * Every span over a range, in order: class, clipped range, flags and value.
 *
 * A later snapshot is compared against it span by span rather than by building
 * its own, because every settled chunk is checked on every snapshot on the
 * measuring thread, and a boxed signature per check is garbage per delta.
 */
internal class SpanSignature private constructor(
  private val classes: Array<Class<*>>,
  /** Start, end and flags of each span, relative to the range. */
  private val ranges: IntArray,
  private val values: Array<Any>,
) {
  fun matches(text: Spanned, from: Int, to: Int): Boolean {
    val spans = text.getSpans(from, to, Any::class.java)
    if (spans.size != classes.size) return false
    for ((index, span) in spans.withIndex()) {
      if (span.javaClass !== classes[index]) return false
      if (maxOf(text.getSpanStart(span), from) - from != ranges[index * 3]) return false
      if (minOf(text.getSpanEnd(span), to) - from != ranges[index * 3 + 1]) return false
      if (text.getSpanFlags(span) != ranges[index * 3 + 2]) return false
      if (!sameValue(span, values[index])) return false
    }
    return true
  }

  companion object {
    /** The spans over text[from, to), or null when one has no comparable value. */
    fun of(text: Spanned, from: Int, to: Int): SpanSignature? {
      val spans = text.getSpans(from, to, Any::class.java)
      val ranges = IntArray(spans.size * 3)
      val values = arrayOfNulls<Any>(spans.size)
      for ((index, span) in spans.withIndex()) {
        values[index] = spanValue(span) ?: return null
        ranges[index * 3] = maxOf(text.getSpanStart(span), from) - from
        ranges[index * 3 + 1] = minOf(text.getSpanEnd(span), to) - from
        ranges[index * 3 + 2] = text.getSpanFlags(span)
      }
      @Suppress("UNCHECKED_CAST")
      return SpanSignature(Array(spans.size) { spans[it].javaClass }, ranges, values as Array<Any>)
    }
  }
}

/**
 * What a span contributes to layout and drawing, for the spans a code body
 * carries. Exact classes only: a subclass may draw something its parent's value
 * does not describe.
 */
private fun spanValue(span: Any): Any? = when (span.javaClass) {
  ForegroundColorSpan::class.java -> (span as ForegroundColorSpan).foregroundColor
  BackgroundColorSpan::class.java -> (span as BackgroundColorSpan).backgroundColor
  AbsoluteSizeSpan::class.java -> (span as AbsoluteSizeSpan).let { if (it.dip) -it.size - 1 else it.size }
  RelativeSizeSpan::class.java -> (span as RelativeSizeSpan).sizeChange
  StyleSpan::class.java -> (span as StyleSpan).style
  StrikethroughSpan::class.java, UnderlineSpan::class.java -> true
  LeadingMarginSpan.Standard::class.java ->
    (span as LeadingMarginSpan.Standard).let { Pair(it.getLeadingMargin(true), it.getLeadingMargin(false)) }
  // A face is compared by its own value only where the platform exposes it; a
  // body is only laid out in chunks where it does.
  TypefaceSpan::class.java -> if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
    (span as TypefaceSpan).let { Pair(it.family, it.typeface) }
  } else null
  else -> (span as? ReusableSpan)?.reuseKey
}

/** [spanValue] compared without boxing a new value for the common spans. */
private fun sameValue(span: Any, value: Any): Boolean = when (span.javaClass) {
  ForegroundColorSpan::class.java -> value is Int && value == (span as ForegroundColorSpan).foregroundColor
  BackgroundColorSpan::class.java -> value is Int && value == (span as BackgroundColorSpan).backgroundColor
  AbsoluteSizeSpan::class.java -> value is Int && value == (span as AbsoluteSizeSpan).let { if (it.dip) -it.size - 1 else it.size }
  TypefaceSpan::class.java -> Build.VERSION.SDK_INT >= Build.VERSION_CODES.P && value is Pair<*, *> &&
    (span as TypefaceSpan).let { value.first == it.family && value.second == it.typeface }
  else -> value == spanValue(span)
}
