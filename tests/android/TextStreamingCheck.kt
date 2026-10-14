package com.downstream

internal const val SEGMENT_TEXT = 0
internal const val SEGMENT_CODE = 2
internal const val CONTAINER_DETAILS = 6
internal class Configuration(var uiMode: Int = 0) {
  companion object {
    const val UI_MODE_NIGHT_MASK = 48
    const val UI_MODE_NIGHT_YES = 32
  }
}
internal class Resources { val configuration = Configuration() }
internal class Context { val resources = Resources() }
internal class LinkVariants(val value: Int = 0) { fun isEmpty() = value == 0; override fun hashCode() = value }
internal data class PlacedCaret(val kind: Int = 0, val place: Int = 0) {
  companion object { val NONE = PlacedCaret() }
}
internal class NativeImages { companion object { val EMPTY = NativeImages() } }
internal object MathFonts { fun attach(context: Context) {} }
internal class NativeTheme {
  var family = ""
  fun familyAt(index: Int) = family
  fun color(slot: Int, night: Boolean): Int? = null
  companion object { fun of(id: Long) = NativeTheme() }
}
internal class Palette(theme: NativeTheme, val night: Boolean) {
  val text = 42
  fun token(tokenClass: Int) = 100 + tokenClass
}
internal class ContainerDecoration(a: Float, b: Float, c: Any?, d: Boolean, e: Any?, f: Float,
                                   g: Float, val kind: Int, val id: Int, val open: Boolean, val summary: Boolean)
internal data class PlannedSegment(val from: Int, val to: Int, val kind: Int,
                                   val identity: Long, val isCommitted: Boolean = true, val settledBlocks: Int = 1,
                                   val isTable: Boolean = false, val isCode: Boolean = kind == SEGMENT_CODE,
                                   val codeOrdinal: Int = if (kind == SEGMENT_CODE) 0 else -1,
                                   val imageOrdinal: Int = -1, val tableOrdinal: Int = -1,
                                   val lastIdentity: Long = identity)
internal open class Segment { var detailsContainers: List<ContainerDecoration> = emptyList() }
internal class CodeSegment(val highlighted: Boolean, val tokenSpans: List<TokenSpan>) : Segment()
internal data class TokenSpan(val color: Int, val start: Int, val end: Int, val flags: Int)
internal class ForegroundColorSpan(val color: Int)
internal class TokenText {
  val spans = mutableListOf<TokenSpan>()
  fun setSpan(span: ForegroundColorSpan, start: Int, end: Int, flags: Int) {
    spans.add(TokenSpan(span.color, start, end, flags))
  }
}
private fun codeTokenSpans(syntaxHighlighting: Boolean): List<TokenSpan> {
  val document = NativeDocument(tokenCount = 2)
  val colors = Palette(NativeTheme(), false)
  val text = TokenText()
  val flags = 33
  /* PRODUCTION_TOKEN_COLORS */
  return text.spans
}
internal object DownstreamNative {
  var tracedId = 0L
  var failedReadId = 0L
  val traceEvents = mutableListOf<String>()
  fun isTracingDocument(id: Long) = id == tracedId
  fun traceDocument(id: Long, event: String) { traceEvents.add(event) }
  val plans = mutableMapOf<Long, List<PlannedSegment>>()
  val documentTexts = mutableMapOf<Long, String>()
  var reads = 0
  var planReads = 0
  val reuseFrom = mutableMapOf<Long, Long>()
  fun readRenderReuseFrom(id: Long) = reuseFrom[id] ?: 0L
  fun readSegmentPlan(id: Long): List<PlannedSegment>? { planReads++; return plans[id] }
  fun readDocument(id: Long, from: Int, to: Int, theme: Long): NativeDocument? {
    reads++
    return if (id == failedReadId) null else NativeDocument(text = documentTexts[id] ?: "$id:$from:$to")
  }
  fun readImages(id: Long) = NativeImages.EMPTY
  fun readLinkUrls(id: Long) = emptyList<String>()
  fun fontScaleCap(theme: Long, scale: Float) = scale
}
internal class NativeDocument(
  val text: String = "text", var blockCount: Int = 1,
  var kind: Int = 0, var contained: Int = 0, var start: Int = 0, var end: Int = text.length,
  var code: Boolean = false, var title: Boolean = false, var tokenCount: Int = 0,
  val formulaTable: DoubleArray = doubleArrayOf(),
  val runValues: List<IntArray> = listOf(intArrayOf(0, -1, -1)),
) {
  val runCount get() = runValues.size
  val previousBottomMargin = 0f
  var fontSize = 16f
  var weight = 400f
  var italic = false
  var family = 0
  var lineHeight = 20f
  var colorSlot = 1
  var direction = 0
  fun blockFontSize(block: Int) = fontSize
  fun blockFontWeight(block: Int) = weight
  fun blockItalic(block: Int) = italic
  fun blockFontFamily(block: Int) = family
  fun blockLineHeight(block: Int) = lineHeight
  fun blockColorSlot(block: Int) = colorSlot
  fun blockDirection(block: Int) = direction
  fun blockIsRtl(block: Int) = direction == 2
  fun blockKind(block: Int) = kind
  fun blockStart(block: Int) = start
  fun blockEnd(block: Int) = end
  fun isCodeElement(block: Int) = code
  fun isAdmonitionTitle(block: Int) = title
  fun runStyles(run: Int) = runValues[run][0]
  fun runLink(run: Int) = runValues[run][1]
  fun runTag(run: Int) = runValues[run][2]
  fun tokenClass(token: Int) = token
  fun tokenStart(token: Int) = token
  fun tokenEnd(token: Int) = token + 1
  fun containerCount(block: Int) = contained
  fun containerKind(block: Int, depth: Int) = 0
  fun containerId(block: Int, depth: Int) = 0
  fun containerOpen(block: Int, depth: Int) = false
  fun showsMarker(block: Int, depth: Int) = false
  companion object { const val KIND_PARAGRAPH = 0; const val CONTAINER_DETAILS = 6; const val TOKEN_PLAIN = 0 }
  /* PRODUCTION_ELIGIBILITY */
}

private data class InlineRun(val styles: Int = 0, val link: Int = -1, val tag: Int = -1)
private class InlineDocument(val runs: List<InlineRun>) {
  val runCount get() = runs.size
  fun runStyles(run: Int) = runs[run].styles
  fun runLink(run: Int) = runs[run].link
  fun runTag(run: Int) = runs[run].tag
}
private fun styledRunIndices(document: InlineDocument, isTable: Boolean): List<Int> {
  val styled = ArrayList<Int>()
  for (run in 0 until document.runCount) {
    /* PRODUCTION_INLINE_GATE */
    // The remaining production body resolves block/font/theme metadata and
    // builds inline spans. Count arrivals at that body, not simulated shaping.
    styled.add(run)
  }
  return styled
}
internal class TextPaint {
  constructor()
  constructor(other: TextPaint)
  val textLocales = Locales()
  var textSize = 16f
  var color = 0
}
internal class StreamingParagraphLayout(val text: String, paint: TextPaint, previous: StreamingParagraphLayout?, lineHeight: Int)
internal class SpannedString(val text: String)
internal object Build {
  object VERSION { const val SDK_INT = 36 }
  object VERSION_CODES { const val P = 28 }
}
internal object PrecomputedTexts {
  var creates = 0
  fun create(text: String, paint: TextPaint, traceId: Long): String { creates++; return text }
}
internal class Locales { var language = "en-US"; fun toLanguageTags() = language }
internal class TextSegment(val text: String) : Segment()
internal class RenderedDocument(val id: Long, val segments: List<Segment>, val plan: List<PlannedSegment>,
                                val paint: Any, val style: Style, val tracing: Boolean = false,
                                private val minimumCacheCost: Long = 0) {
  val documentId get() = id
  // Use the production charge method with a fixture payload-cost estimator.
  /* PRODUCTION_RENDER_COST */
  fun retainedCostForCheck() = cacheCost()
  internal class Style(val scale: Float, val colors: Palette, val theme: NativeTheme, val themeId: Long) {
    val paint = TextPaint()
    val density = 1f
    var bodyFamily = ""
    var bodyWeight = 400f
    var bodyItalic = false
    fun gapAbove(document: NativeDocument, block: Int, previous: Float) = previous
    fun px(value: Float) = value
  }
  companion object {
    var builds = 0
    private val cache = RenderingCache<String, RenderedDocument>(128, 1024) { 1 }
    private val committedSegments = RenderingCache<String, Segment>(128, 1024) { 1 }
    private fun segmentCost(segment: Segment): Long = if (segment is TextSegment) segment.text.length * 24L + 256 else 512
    private fun buildSegment(document: NativeDocument, images: NativeImages, style: Style, planned: PlannedSegment,
      ordinal: Int, tableOrdinal: Int, highlights: Boolean, bottom: Float, caret: PlacedCaret,
      variants: LinkVariants, urls: List<String>, traceId: Long, incrementalParagraphs: Boolean = false): Segment {
      builds++
      return if (planned.kind == SEGMENT_CODE) CodeSegment(highlights, codeTokenSpans(highlights)) else TextSegment(document.text)
    }
    /* PRODUCTION_OBTAIN */
    private val plainParagraphs = RenderingCache<PlainParagraphKey, TextSegment>(128, 2L * 1024 * 1024) {
      it.text.length * 24L + 256
    }
    var paragraphBuilds = 0
    /* PRODUCTION_PARAGRAPH_LANES */
    private fun seedParagraphLane(lane: Int): java.lang.ref.WeakReference<PlainParagraphKey> {
      val content = lane.toString().padStart(3, '0') + "x".repeat(32765)
      val key = PlainParagraphKey(content, listOf(1L, 1f, "en-US"))
      cachePlainParagraph(key, lane, TextSegment(content))
      return java.lang.ref.WeakReference(key)
    }
    fun checkParagraphLaneRetention() {
      plainParagraphs.removeAll { true }
      plainParagraphLanes.clear()
      val references = List(128) { seedParagraphLane(it) }
      repeat(30) {
        if (references.take(126).any { it.get() != null }) System.gc()
      }
      check(references.take(126).all { it.get() == null }) {
        "paragraph lanes retained evicted content: ${references.count { it.get() != null }} of 128 keys alive"
      }
      for (reference in references.takeLast(2)) {
        val key = checkNotNull(reference.get()) { "live cache key lost its owner" }
        check(plainParagraphs.get(key)?.text == key.content)
      }
      plainParagraphs.removeAll { true }
      plainParagraphLanes.clear()
      println("production paragraph lanes: 126 evicted keys collect; 2 budget-owned keys remain reusable")
    }
    /* PRODUCTION_PARAGRAPH_KEY */
    /* PRODUCTION_STREAM_ELIGIBILITY */
    private val streamingParagraphs = StreamingParagraphSeeds<StreamingParagraphLayout>(16, 2L * 1024 * 1024)
    fun layoutPolicy(document: NativeDocument, style: Style, planned: PlannedSegment,
                     caret: PlacedCaret = PlacedCaret.NONE, incrementalParagraphs: Boolean = true): Pair<Boolean, Any> {
      val paint = style.paint
      val theme = style.theme
      val colors = style.colors
      val text = document.text
      val traceId = 0L
      val isCode = false
      fun px(dp: Float) = style.px(dp)
      /* PRODUCTION_STREAM_PREPARE */
      return Pair(streamingParagraph != null, displayText)
    }
    fun paragraph(document: NativeDocument, style: Style, planned: PlannedSegment,
                  caret: PlacedCaret = PlacedCaret.NONE, previousBottom: Float = 0f,
                  incrementalParagraphs: Boolean = false): TextSegment {
      val traceId = 0L
      /* PRODUCTION_PARAGRAPH_LOOKUP */
      paragraphBuilds++
      return TextSegment(document.text).also { segment -> plainKey?.let { cachePlainParagraph(it, planned.from, segment) } }
    }
  }
}

private object View { const val GONE = 8; const val VISIBLE = 0 }
private class Visibility { var visibility = View.VISIBLE }
private class CardBackground { fun setColor(color: Int) {} }
private class CodeColors { val codeBlockBackground = 1 }
private class HostedCodeSegment(val source: String) {
  var preparations = 0
  val text by lazy { preparations++; source }
  val accessibleBlocks = emptyList<String>()
  val colors = CodeColors()
}
private class Body {
  var bindings = 0
  var shown = ""
  fun clearContent() { shown = "" }
  fun setContent(text: String, accessibleBlocks: List<String>, rendered: RenderedDocument) {
    bindings++; shown = text
  }
}
private class HostedCode(val rendered: RenderedDocument) {
  private var hosted = false
  var segment: HostedCodeSegment? = null
  private var boundSegment: HostedCodeSegment? = null
  val body = Body()
  val viewport = Visibility()
  val gutter = Visibility()
  val cardBackground = CardBackground()
  fun requestLayout() {}
  fun invalidate() {}
  fun applyHeaderVisibility() {}
  fun update(next: HostedCodeSegment) { segment = next; if (!hosted) bindFallback() }
  /* PRODUCTION_HOSTED */
  /* PRODUCTION_BIND_FALLBACK */
}
fun main() {
  RenderedDocument.checkParagraphLaneRetention()
  val plainRuns = InlineDocument(List(4096) { InlineRun() })
  check(styledRunIndices(plainRuns, false).isEmpty())
  check(styledRunIndices(plainRuns, true) == (0 until 4096).toList())
  val decorated = InlineDocument(listOf(
    InlineRun(), InlineRun(styles = 1), InlineRun(styles = Int.MIN_VALUE),
    InlineRun(link = 0), InlineRun(tag = 0), InlineRun(styles = -1), InlineRun(),
  ))
  check(styledRunIndices(decorated, false) == listOf(1, 2, 3, 4, 5))
  println("production inline gate: 4096 plain runs, 0 block/font/theme walks; table, links, tags, every style bit preserve fallback")
  val context = Context()
  DownstreamNative.plans[1] = listOf(PlannedSegment(0, 1, 0, 11))
  val prepared = RenderedDocument.obtain(context, 1, 1f, true)!!
  repeat(100) {
    check(RenderedDocument.obtain(context, 1, 1f, false) === prepared)
    check(RenderedDocument.obtain(context, 1, 1f, true) === prepared)
  }
  check(RenderedDocument.builds == 1 && DownstreamNative.reads == 1 && DownstreamNative.planReads == 1)
  DownstreamNative.plans[2] = listOf(PlannedSegment(0, 1, 0, 11), PlannedSegment(1, 2, 2, 22))
  val highlighted = RenderedDocument.obtain(context, 2, 1f, true)!!
  val unhighlighted = RenderedDocument.obtain(context, 2, 1f, false)!!
  check(highlighted !== unhighlighted)
  check(highlighted.segments[0] === prepared.segments[0] && unhighlighted.segments[0] === prepared.segments[0])
  check(highlighted.segments[1] !== unhighlighted.segments[1])
  check(RenderedDocument.builds == 3 && DownstreamNative.reads == 3)
  check(RenderedDocument.obtain(context, 1, 1.5f, true) !== prepared)
  check(RenderedDocument.obtain(context, 1, 1f, true, themeId = 7) !== prepared)
  check(RenderedDocument.obtain(context, 1, 1f, true, caret = PlacedCaret(1, 1)) !== prepared)
  // A revoked provisional block is replaced under an unchanged first block, so a
  // committed chunk with the same start and length but a new last block rebuilds.
  DownstreamNative.plans[3] = listOf(PlannedSegment(0, 2, 0, 31, lastIdentity = 32))
  val provisional = RenderedDocument.obtain(context, 3, 1f, true)!!
  DownstreamNative.plans[4] = listOf(PlannedSegment(0, 2, 0, 31, lastIdentity = 33))
  check(RenderedDocument.obtain(context, 4, 1f, true)!!.segments[0] !== provisional.segments[0])
  DownstreamNative.plans[5] = listOf(PlannedSegment(0, 2, 0, 31, lastIdentity = 32))
  check(RenderedDocument.obtain(context, 5, 1f, true)!!.segments[0] === provisional.segments[0])
  val mixedLivePlan = listOf(
    PlannedSegment(0, 1, SEGMENT_TEXT, 0, false, 0),
    PlannedSegment(1, 2, SEGMENT_CODE, 0, false, 0),
    PlannedSegment(2, 3, SEGMENT_TEXT, 0, false, 0),
  )
  val heldRenderings = mutableListOf<RenderedDocument>()
  for ((index, firstHighlight) in listOf(true, false).withIndex()) {
    val id = 100L + index
    DownstreamNative.plans[id] = mixedLivePlan
    val startReads = DownstreamNative.reads
    val startBuilds = RenderedDocument.builds
    val first = RenderedDocument.obtain(context, id, 1f, firstHighlight)!!
    val second = RenderedDocument.obtain(context, id, 1f, !firstHighlight)!!
    check(first !== second)
    check(first.segments[0] === second.segments[0])
    check(first.segments[2] === second.segments[2])
    check(first.segments[1] !== second.segments[1])
    check((first.segments[1] as CodeSegment).highlighted == firstHighlight)
    check((second.segments[1] as CodeSegment).highlighted == !firstHighlight)
    check((first.segments[1] as CodeSegment).tokenSpans ==
      if (firstHighlight) listOf(TokenSpan(101, 1, 2, 33)) else emptyList<TokenSpan>())
    check((second.segments[1] as CodeSegment).tokenSpans ==
      if (!firstHighlight) listOf(TokenSpan(101, 1, 2, 33)) else emptyList<TokenSpan>())
    check(DownstreamNative.reads - startReads == 4)
    check(RenderedDocument.builds - startBuilds == 4)
    check(RenderedDocument.obtain(context, id, 1f, firstHighlight) === first)
    heldRenderings.add(first)
  }
  println("production mixed live cache: both highlight request orders share provisional prose; 4 JNI reads/builds for 6 segments, separate code color variants")
  val changedSettings: List<(Context, Long, Boolean) -> RenderedDocument?> = listOf(
    { ctx, id, highlight -> RenderedDocument.obtain(ctx, id, 1.5f, highlight) },
    { _, id, highlight ->
      val dark = Context().apply { resources.configuration.uiMode = Configuration.UI_MODE_NIGHT_YES }
      RenderedDocument.obtain(dark, id, 1f, highlight)
    },
    { ctx, id, highlight -> RenderedDocument.obtain(ctx, id, 1f, highlight, themeId = 7) },
    { ctx, id, highlight -> RenderedDocument.obtain(ctx, id, 1f, highlight, caret = PlacedCaret(1, 1)) },
    { ctx, id, highlight -> RenderedDocument.obtain(ctx, id, 1f, highlight, caret = PlacedCaret(0, 1)) },
    { ctx, id, highlight -> RenderedDocument.obtain(ctx, id, 1f, highlight, linkVariants = LinkVariants(7)) },
    { ctx, id, highlight -> RenderedDocument.obtain(ctx, id, 1f, highlight, incrementalParagraphs = true) },
  )
  for ((order, firstHighlight) in listOf(true, false).withIndex()) {
    for ((index, changed) in changedSettings.withIndex()) {
      val id = 200L + order * 100 + index
      DownstreamNative.plans[id] = mixedLivePlan
      val startReads = DownstreamNative.reads
      val first = RenderedDocument.obtain(context, id, 1f, firstHighlight)!!
      val second = changed(context, id, !firstHighlight)!!
      check(first.segments.indices.all { first.segments[it] !== second.segments[it] })
      check(DownstreamNative.reads - startReads == 6)
    }
  }
  val changedPlans = listOf(
    mixedLivePlan[0].copy(from = 1), mixedLivePlan[0].copy(to = 4),
    mixedLivePlan[0].copy(isTable = true), mixedLivePlan[0].copy(isCommitted = true),
    mixedLivePlan[0].copy(isCode = true), mixedLivePlan[0].copy(identity = 900),
    mixedLivePlan[0].copy(settledBlocks = 1), mixedLivePlan[0].copy(codeOrdinal = 3),
    mixedLivePlan[0].copy(kind = 4), mixedLivePlan[0].copy(imageOrdinal = 3),
    mixedLivePlan[0].copy(tableOrdinal = 3),
  ).map { listOf(it) + mixedLivePlan.drop(1) } + listOf(mixedLivePlan.take(2))
  for ((index, changedPlan) in changedPlans.withIndex()) {
    val id = 400L + index
    DownstreamNative.plans[id] = mixedLivePlan
    val first = RenderedDocument.obtain(context, id, 1f, true)!!
    // A native snapshot is immutable. Simulate a contradictory JNI plan to
    // prove the reuse path rejects it rather than trusting identity alone.
    DownstreamNative.plans[id] = changedPlan
    val second = RenderedDocument.obtain(context, id, 1f, false)!!
    check(first.segments[0] !== second.segments[0])
  }
  DownstreamNative.plans[500] = mixedLivePlan
  val otherSnapshot = RenderedDocument.obtain(context, 500, 1f, false)!!
  check(heldRenderings[0].segments[0] !== otherSnapshot.segments[0])
  repeat(130) { index ->
    val id = 600L + index
    DownstreamNative.plans[id] = listOf(mixedLivePlan[0])
    RenderedDocument.obtain(context, id, 1f, false)!!
  }
  for (held in heldRenderings) {
    check((held.segments[0] as TextSegment).text == "${held.id}:0:1")
    check((held.segments[2] as TextSegment).text == "${held.id}:2:3")
    val highlight = (held.segments[1] as CodeSegment).highlighted
    check(RenderedDocument.obtain(context, held.id, 1f, highlight) !== held)
    check((held.segments[1] as CodeSegment).highlighted == highlight)
  }
  println("production mixed live isolation: font scale, appearance, theme, caret kind/place, links, paragraph policy, snapshot id and every plan field reject sharing; held segments survive eviction")
  val prosePlan = PlannedSegment(0, 1, 0, 0, false)
  val proseStyle = RenderedDocument.Style(1f, Palette(NativeTheme(), false), NativeTheme(), 0L)
  val prose = NativeDocument()
  val immutableParagraph = RenderedDocument.paragraph(prose, proseStyle, prosePlan)
  repeat(100) { check(RenderedDocument.paragraph(NativeDocument(), proseStyle, prosePlan) === immutableParagraph) }
  check(RenderedDocument.paragraphBuilds == 1)
  check(RenderedDocument.paragraph(NativeDocument("text grows"), proseStyle, prosePlan) !== immutableParagraph)
  check(immutableParagraph.text == "text")
  for (changed in listOf(
    NativeDocument().apply { fontSize++ }, NativeDocument().apply { weight++ },
    NativeDocument().apply { italic = true }, NativeDocument().apply { family++ },
    NativeDocument().apply { lineHeight++ }, NativeDocument().apply { colorSlot++ },
    NativeDocument().apply { direction++ }, NativeDocument().apply { runValues[0][0] = 1 },
  )) check(RenderedDocument.paragraph(changed, proseStyle, prosePlan) !== immutableParagraph)
  check(RenderedDocument.paragraph(prose, proseStyle, prosePlan, PlacedCaret(1, 1)) !== immutableParagraph)
  check(RenderedDocument.paragraph(prose, proseStyle, prosePlan, previousBottom = 20f) !== immutableParagraph)
  proseStyle.paint.textLocales.language = "ar"
  check(RenderedDocument.paragraph(prose, proseStyle, prosePlan) !== immutableParagraph)
  // A growing paragraph evicts its own prefix; another lane, or a paragraph that
  // is not an extension, keeps its entry.
  proseStyle.paint.textLocales.language = "en"
  val lanePlan = PlannedSegment(7, 8, 0, 0, false)
  val otherLane = PlannedSegment(9, 10, 0, 0, false)
  val short = RenderedDocument.paragraph(NativeDocument("grow"), proseStyle, lanePlan)
  val elsewhere = RenderedDocument.paragraph(NativeDocument("grower"), proseStyle, otherLane)
  val longer = RenderedDocument.paragraph(NativeDocument("grow more"), proseStyle, lanePlan)
  check(RenderedDocument.paragraph(NativeDocument("grow more"), proseStyle, lanePlan) === longer)
  check(RenderedDocument.paragraph(NativeDocument("grower"), proseStyle, otherLane) === elsewhere)
  val builds = RenderedDocument.paragraphBuilds
  check(RenderedDocument.paragraph(NativeDocument("grow"), proseStyle, lanePlan) !== short)
  check(RenderedDocument.paragraphBuilds == builds + 1 && short.text == "grow")
  val sibling = RenderedDocument.paragraph(NativeDocument("unrelated"), proseStyle, lanePlan)
  check(RenderedDocument.paragraph(NativeDocument("grow"), proseStyle, lanePlan).text == "grow")
  check(RenderedDocument.paragraph(NativeDocument("unrelated"), proseStyle, lanePlan) === sibling)
  println("production plain cache: 101 identical active paragraphs, 1 span/preparation build; growth/styles/metrics/caret/locale/gap invalidate; growing prefixes evicted per lane")
  println("production obtain: 201 prose requests, 1 JNI block read/build; mixed code flags share prose and rebuild code")

  val card = HostedCode(prepared)
  card.setHosted(true)
  val versions = List(100) { HostedCodeSegment("code".repeat(it + 1)) }
  versions.forEach { card.update(it) }
  check(card.body.bindings == 0 && versions.all { it.preparations == 0 })
  card.setHosted(false)
  check(card.body.bindings == 1 && card.body.shown == versions.last().source)
  check(versions.last().preparations == 1 && card.viewport.visibility == View.VISIBLE)
  card.setHosted(true)
  check(card.body.shown.isEmpty())
  card.setHosted(false)
  check(card.body.bindings == 2 && versions.last().preparations == 1)
  println("production hosted fallback: 100 revisions, 0 bindings/preparations; unmount binds latest content immediately")

  check(NativeDocument().isPlainParagraph())
  for (document in listOf(
    NativeDocument(kind = 1), NativeDocument(blockCount = 2), NativeDocument(contained = 1),
    NativeDocument(start = 1), NativeDocument(end = 1), NativeDocument(code = true),
    NativeDocument(title = true), NativeDocument(tokenCount = 1), NativeDocument(formulaTable = doubleArrayOf(1.0)),
    NativeDocument(runValues = listOf(intArrayOf(1, -1, -1))),
    NativeDocument(runValues = listOf(intArrayOf(0, 0, -1))),
    NativeDocument(runValues = listOf(intArrayOf(0, -1, 0))),
  )) check(!document.isPlainParagraph())

  val policyStyle = RenderedDocument.Style(1f, Palette(NativeTheme.of(0), false), NativeTheme.of(0), 0)
  val livePlan = PlannedSegment(0, 1, SEGMENT_TEXT, 0, false, 0)
  val liveDocument = NativeDocument(text = "plain ASCII paragraph")
  PrecomputedTexts.creates = 0
  val live = RenderedDocument.layoutPolicy(liveDocument, policyStyle, livePlan)
  check(live.first && live.second is SpannedString && PrecomputedTexts.creates == 0)
  val finished = RenderedDocument.layoutPolicy(liveDocument, policyStyle, livePlan.copy(settledBlocks = 1))
  check(!finished.first && PrecomputedTexts.creates == 1)
  check(!RenderedDocument.layoutPolicy(liveDocument, policyStyle, livePlan, incrementalParagraphs = false).first)
  check(!RenderedDocument.layoutPolicy(liveDocument, policyStyle, livePlan.copy(isCommitted = true)).first)
  check(!RenderedDocument.layoutPolicy(liveDocument, policyStyle, livePlan.copy(to = 2)).first)
  check(!RenderedDocument.layoutPolicy(liveDocument, policyStyle, livePlan, PlacedCaret(1)).first)
  for (document in listOf(
    NativeDocument(text = "emoji \uD83D\uDE42"), NativeDocument(text = "Arabic مرحبا"),
    NativeDocument(text = "two\nlines"), NativeDocument(text = "tab\tstop"), NativeDocument(text = ""),
    NativeDocument(text = "styled", runValues = listOf(intArrayOf(1, -1, -1))),
    NativeDocument(text = "linked", runValues = listOf(intArrayOf(0, 0, -1))),
    NativeDocument(text = "tagged", runValues = listOf(intArrayOf(0, -1, 0))),
    NativeDocument(text = "contained", contained = 1),
    NativeDocument(text = "RTL plain").apply { direction = 2 },
    NativeDocument(text = "bold plain").apply { weight = 700f },
    NativeDocument(text = "italic plain").apply { italic = true },
  )) check(!RenderedDocument.layoutPolicy(document, policyStyle, livePlan).first)
  policyStyle.theme.family = "custom-font"
  check(!RenderedDocument.layoutPolicy(liveDocument, policyStyle, livePlan).first)
  policyStyle.theme.family = ""
  policyStyle.bodyFamily = "custom-font"
  check(!RenderedDocument.layoutPolicy(liveDocument, policyStyle, livePlan).first)
  policyStyle.bodyFamily = ""
  policyStyle.bodyWeight = 500f
  check(!RenderedDocument.layoutPolicy(liveDocument, policyStyle, livePlan).first)
  policyStyle.bodyWeight = 400f
  policyStyle.bodyItalic = true
  check(!RenderedDocument.layoutPolicy(liveDocument, policyStyle, livePlan).first)
  policyStyle.bodyItalic = false
  val cachedLive = RenderedDocument.paragraph(liveDocument, policyStyle, livePlan, incrementalParagraphs = true)
  check(RenderedDocument.paragraph(liveDocument, policyStyle, livePlan, incrementalParagraphs = true) === cachedLive)
  check(RenderedDocument.paragraph(liveDocument, policyStyle, livePlan.copy(settledBlocks = 1), incrementalParagraphs = true) !== cachedLive)
  check(RenderedDocument.paragraph(liveDocument, policyStyle, livePlan) !== cachedLive)
  DownstreamNative.plans[9000] = listOf(livePlan)
  val modeCanonical = RenderedDocument.obtain(Context(), 9000, 1f)!!
  val modeIncremental = RenderedDocument.obtain(Context(), 9000, 1f, incrementalParagraphs = true)!!
  check(modeCanonical !== modeIncremental)
  check(RenderedDocument.obtain(Context(), 9000, 1f) === modeCanonical)
  check(RenderedDocument.obtain(Context(), 9000, 1f, incrementalParagraphs = true) === modeIncremental)
  DownstreamNative.plans[9001] = listOf(PlannedSegment(0, 1, SEGMENT_CODE, 80001, true, 1))
  val canonicalCode = RenderedDocument.obtain(Context(), 9001, 1f)!!
  val incrementalCode = RenderedDocument.obtain(Context(), 9001, 1f, incrementalParagraphs = true)!!
  check(canonicalCode.segments.single() === incrementalCode.segments.single())
  println("production incremental policy: live ASCII bypasses PrecomputedText; finish, modes, RTL, Unicode, caret, containers, spans and custom typography preserve canonical fallback")

  val branches = StreamingParagraphSeeds<String>(16, 2L * 1024 * 1024)
  var a = "First stream "; var b = "Second stream "
  branches.put("same metrics", a, a)
  branches.put("same metrics", b, b)
  repeat(1536) {
    val nextA = a + "plain streaming word a "
    check(branches.matching("same metrics", nextA) == a)
    branches.put("same metrics", nextA, nextA)
    a = nextA
    val nextB = b + "plain streaming word b "
    check(branches.matching("same metrics", nextB) == b)
    branches.put("same metrics", nextB, nextB)
    b = nextB
  }
  check(branches.matching("other metrics", a + "more") == null)
  val tiny = StreamingParagraphSeeds<String>(2, 4096)
  tiny.put("key", "first", "first"); tiny.put("key", "second", "second")
  tiny.put("key", "third", "third")
  check(tiny.matching("key", "first longer") == null)
  check(tiny.matching("key", "second longer") == "second")
  tiny.put("key", "x".repeat(8192), "oversized")
  check(tiny.matching("key", "x".repeat(8193)) == null)
  println("production paragraph branches: 2 interleaved 33 KiB streams retain independent append seeds; metric mismatch, count eviction and oversized rejection passed")

  // Exercise the actual production obtain() with a native-proven finish alias.
  DownstreamNative.plans[9200] = listOf(PlannedSegment(0, 1, SEGMENT_TEXT, 0, false, 0))
  DownstreamNative.plans[9201] = listOf(PlannedSegment(0, 1, SEGMENT_TEXT, 0, false, 1))
  DownstreamNative.reuseFrom[9201] = 9200
  val liveRendering = RenderedDocument.obtain(context, 9200, 1f)!!
  val beforeFinishBuilds = RenderedDocument.builds
  val beforeFinishReads = DownstreamNative.reads
  val finalRendering = RenderedDocument.obtain(context, 9201, 1f)!!
  check(finalRendering !== liveRendering && finalRendering.documentId == 9201L)
  check(finalRendering.plan.single().settledBlocks == 1 && liveRendering.plan.single().settledBlocks == 0)
  check(finalRendering.segments.single() === liveRendering.segments.single())
  check(RenderedDocument.builds == beforeFinishBuilds && DownstreamNative.reads == beforeFinishReads)
  check(RenderedDocument.obtain(context, 9201, 1f, caret = PlacedCaret(1, 1))!!.segments.single() !== liveRendering.segments.single())
  check(RenderedDocument.obtain(context, 9201, 2f)!!.segments.single() !== liveRendering.segments.single())
  check(RenderedDocument.obtain(context, 9201, 1f, themeId = 44)!!.segments.single() !== liveRendering.segments.single())
  check(RenderedDocument.obtain(context, 9201, 1f, linkVariants = LinkVariants(7))!!.segments.single() !== liveRendering.segments.single())
  check(RenderedDocument.obtain(context, 9201, 1f, incrementalParagraphs = true)!!.segments.single() !== liveRendering.segments.single())
  DownstreamNative.plans[9202] = DownstreamNative.plans[9201]!!
  DownstreamNative.reuseFrom[9202] = 999999
  check(RenderedDocument.obtain(context, 9202, 1f)!!.segments.single() !== liveRendering.segments.single())
  println("production finish reuse: final wrapper/settlement refreshed; identity-zero tail 0 JNI reads/builds; options, canonical paragraph lane and missing parent reject reuse")

  DownstreamNative.plans[9300] = listOf(PlannedSegment(0, 1, SEGMENT_TEXT, 0, false, 0))
  DownstreamNative.documentTexts[9300] = "x".repeat(100_000)
  DownstreamNative.plans[9301] = listOf(PlannedSegment(0, 1, SEGMENT_TEXT, 9301, true, 1))
  DownstreamNative.reuseFrom[9301] = 9300
  val largeLive = RenderedDocument.obtain(context, 9300, 1f)!!
  val largeFinal = RenderedDocument.obtain(context, 9301, 1f)!!
  check(largeFinal.segments.single() === largeLive.segments.single())
  check(largeFinal.retainedCostForCheck() == largeLive.retainedCostForCheck())
  check(largeFinal.retainedCostForCheck() > 2_400_000)
  val charged = RenderingCache<Long, RenderedDocument>(128, 8L * 1024 * 1024) { it.retainedCostForCheck() }
  repeat(20) { charged.put(it.toLong(), largeFinal) }
  check(charged.get(0) == null && charged.get(19) === largeFinal)
  println("production finish retention: newly committed shared tail remains charged; 8 MiB cache evicts repeated aliases")

  check(DownstreamNative.traceEvents.isEmpty()) { "disabled tracing must emit no events" }
  DownstreamNative.tracedId = 9100
  DownstreamNative.plans[9100] = listOf(PlannedSegment(0, 1, 0, 9100))
  val traced = RenderedDocument.obtain(context, 9100, 1f)!!
  check(DownstreamNative.traceEvents == listOf(
    "android_render_build_start", "android_segment_build", "android_render_build_end",
  )) { "a rendering miss must report a balanced build and segment count" }
  DownstreamNative.traceEvents.clear()
  check(RenderedDocument.obtain(context, 9100, 1f) === traced)
  check(RenderedDocument.obtain(context, 9100, 1f, false) === traced)
  check(DownstreamNative.traceEvents == List(2) { "android_render_cache_hit" }) {
    "normalized code-free cache hits must not look like rebuilding"
  }
  DownstreamNative.traceEvents.clear()
  DownstreamNative.tracedId = 9101
  DownstreamNative.plans[9101] = listOf(PlannedSegment(0, 1, 0, 9100))
  RenderedDocument.obtain(context, 9101, 1f)!!
  check(DownstreamNative.traceEvents == listOf(
    "android_render_build_start", "android_segment_reuse", "android_render_build_end",
  )) { "a committed segment hit must report reuse" }
  DownstreamNative.traceEvents.clear()
  DownstreamNative.tracedId = 9102
  DownstreamNative.failedReadId = 9102
  DownstreamNative.plans[9102] = listOf(PlannedSegment(0, 1, 0, 9102))
  check(RenderedDocument.obtain(context, 9102, 1f) == null)
  check(DownstreamNative.traceEvents.first() == "android_render_build_start" &&
    DownstreamNative.traceEvents.last() == "android_render_build_end") {
    "an unavailable document must not leave an unmatched build duration"
  }
  println("production tracing: disabled path silent, cache hits and segment reuse counted, failed builds balanced")

}
