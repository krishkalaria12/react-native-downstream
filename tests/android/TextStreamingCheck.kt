package com.downstream

internal const val SEGMENT_TEXT = 0
internal const val SEGMENT_CODE = 2
internal const val CONTAINER_DETAILS = 6
internal object Configuration {
  const val UI_MODE_NIGHT_MASK = 48
  const val UI_MODE_NIGHT_YES = 32
  val uiMode = 0
}
internal class Resources { val configuration = Configuration }
internal class Context { val resources = Resources() }
internal class LinkVariants { fun isEmpty() = true; override fun hashCode() = 0 }
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
internal class Palette(theme: NativeTheme, val night: Boolean) { val text = 42 }
internal class ContainerDecoration(a: Float, b: Float, c: Any?, d: Boolean, e: Any?, f: Float,
                                   g: Float, val kind: Int, val id: Int, val open: Boolean, val summary: Boolean)
internal data class PlannedSegment(val from: Int, val to: Int, val kind: Int,
                                   val identity: Long, val isCommitted: Boolean = true, val settledBlocks: Int = 1) {
  val codeOrdinal = if (kind == SEGMENT_CODE) 0 else -1
  val tableOrdinal = -1
}
internal open class Segment { var detailsContainers: List<ContainerDecoration> = emptyList() }
internal object DownstreamNative {
  fun isTracingDocument(id: Long) = false
  fun traceDocument(id: Long, event: String) {}
  val plans = mutableMapOf<Long, List<PlannedSegment>>()
  var reads = 0
  var planReads = 0
  fun readSegmentPlan(id: Long): List<PlannedSegment>? { planReads++; return plans[id] }
  fun readDocument(id: Long, from: Int, to: Int, theme: Long): NativeDocument { reads++; return NativeDocument() }
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
  fun containerCount(block: Int) = contained
  fun containerKind(block: Int, depth: Int) = 0
  fun containerId(block: Int, depth: Int) = 0
  fun containerOpen(block: Int, depth: Int) = false
  fun showsMarker(block: Int, depth: Int) = false
  companion object { const val KIND_PARAGRAPH = 0; const val CONTAINER_DETAILS = 6 }
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
                                paint: Any, style: Style, tracing: Boolean = false) {
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
    private fun buildSegment(document: NativeDocument, images: NativeImages, style: Style, planned: PlannedSegment,
      ordinal: Int, tableOrdinal: Int, highlights: Boolean, bottom: Float, caret: PlacedCaret,
      variants: LinkVariants, urls: List<String>, traceId: Long, incrementalParagraphs: Boolean = false): Segment { builds++; return Segment() }
    /* PRODUCTION_OBTAIN */
    private val plainParagraphs = RenderingCache<PlainParagraphKey, TextSegment>(128, 2L * 1024 * 1024) {
      it.text.length * 24L + 256
    }
    var paragraphBuilds = 0
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
      return TextSegment(document.text).also { segment -> plainKey?.let { plainParagraphs.put(it, segment) } }
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
  println("production plain cache: 101 identical active paragraphs, 1 span/preparation build; growth/styles/metrics/caret/locale/gap invalidate")
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

}
