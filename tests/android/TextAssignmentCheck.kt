package com.downstream

private const val CONTAINER_DETAILS = 6
private class Spanned(private val value: String) : CharSequence by value {
  override fun toString() = value
}
private data class AccessibleBlock(val start: Int, val end: Int, val heading: Boolean = false, val taskState: Int = 0)
private data class Container(val kind: Int, val id: Int)
private data class DetailsBlock(val start: Int, val end: Int, val summary: Boolean, val containers: List<Container>)
private data class AccessibleTextPiece(val start: Int, val end: Int)
private data class TextReadingPart(val piece: AccessibleTextPiece, val heading: Boolean, val taskState: Int,
                                 val summaryContainer: Int? = null, val isSummary: Boolean = false)
private fun linkPieces(text: Spanned, start: Int, end: Int) = listOf(AccessibleTextPiece(start, end))
private class StreamingParagraphFrame(val height: Int)
private class StreamingParagraph {
  var warm = true
  fun frame(width: Int): StreamingParagraphFrame? = if (warm) StreamingParagraphFrame(40) else null
}
private class TextSegment(val text: Spanned, val accessibleBlocks: List<AccessibleBlock>,
                          val detailsBlocks: List<DetailsBlock> = emptyList(),
                          val streamingParagraph: StreamingParagraph? = null)
private class Paint {
  var typeface = "body"
  var textSize = 16f
  var color = 42
  fun set(other: Paint) { typeface = other.typeface; textSize = other.textSize; color = other.color }
}
private class Colors(val selectionColor: Int? = 17)
private class Style(val colors: Colors = Colors())
private class RenderedDocument(val paint: Paint = Paint(), val style: Style = Style(),
                               val tracing: Boolean = true, val documentId: Long = 9)
private object Build {
  object VERSION { var SDK_INT = 36 }
  object VERSION_CODES { const val P = 28 }
}
private object TypedValue { const val COMPLEX_UNIT_PX = 0 }
private enum class BufferType { SPANNABLE, NORMAL }
private object View {
  object MeasureSpec { fun getSize(spec: Int) = spec }
  fun resolveSize(value: Int, spec: Int) = value
}
private object DownstreamNative {
  val events = mutableListOf<String>()
  fun traceDocument(id: Long, event: String) { events.add(event) }
}
private object PrecomputedTexts {
  var enabled = false
  var calls = 0
  fun show(view: AssignmentView, text: Spanned, color: Int): Boolean {
    calls++
    if (enabled) view.text = text
    return enabled
  }
}
private class Accessibility {
  var notifications = 0
  fun contentChanged() { notifications++ }
}
private open class PlatformView {
  var assignments = 0
  var text: CharSequence = ""
    set(value) { assignments++; field = value }
  var isTextSelectable = false
  val paint = Paint()
  var width = 0
  val paddingLeft = 0
  val paddingRight = 0
  val paddingTop = 0
  val paddingBottom = 0
  var highlightColor = 0
  var buffer: BufferType? = null
  fun setText(text: Spanned, buffer: BufferType) { this.buffer = buffer; this.text = text }
  fun setTextSize(unit: Int, size: Float) { paint.textSize = size }
  fun setTextColor(color: Int) { paint.color = color }
  fun setTextIsSelectable(value: Boolean) { isTextSelectable = value }
  fun requestLayout() {}
  fun invalidate() {}
  fun setMeasuredDimension(width: Int, height: Int) {}
  open fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {}
}
private class AssignmentView : PlatformView() {
  private var segment: TextSegment? = null
  private var blockOffset = 0
  private var streamingFrame: StreamingParagraphFrame? = null
  private var streamingRendered: RenderedDocument? = null
  private var streamingCanonicalAssigned = false
  private var summary: DetailsBlock? = null
  private var spoilersRevealed = false
  private var bandFrame: Any? = null
  private var readingBlocks = emptyList<AccessibleBlock>()
  private var readingParts = emptyList<TextReadingPart>()
  val accessibility = Accessibility()
  var readingBuilds = 0
  var lastBlocks = emptyList<AccessibleBlock>()
  var lastDetails = emptyList<DetailsBlock>()
  private fun installSelectionCallback() {}
  private fun buildReadingParts(blocks: List<AccessibleBlock>, details: List<DetailsBlock>): List<TextReadingPart> {
    readingBuilds++
    lastBlocks = blocks
    lastDetails = details
    return productionReadingParts(blocks, details)
  }
  fun checkSegment(expected: TextSegment?, offset: Int) {
    check(segment === expected && blockOffset == offset)
    check(readingBlocks === expected?.accessibleBlocks || expected == null)
  }
  fun checkDetails(expected: DetailsBlock?) {
    check(summary === expected)
    if (expected != null) {
      check(readingParts.single().summaryContainer == expected.containers.single().id)
      check(readingParts.single().isSummary)
      check(readingParts.single().heading && readingParts.single().taskState == 2)
    } else {
      check(readingParts.all { it.summaryContainer == null && !it.isSummary })
    }
  }
  fun checkStreaming(expected: RenderedDocument?, assigned: Boolean) {
    check(streamingRendered === expected && streamingCanonicalAssigned == assigned)
  }
  fun checkReadingRange(end: Int) {
    check(readingParts.single().piece == AccessibleTextPiece(0, end))
  }
  /* PRODUCTION_displayText */
  /* PRODUCTION_buildReadingParts */
  /* PRODUCTION_setSegment */
  /* PRODUCTION_setContent */
  /* PRODUCTION_assignContent */
  /* PRODUCTION_setSelectable */
  /* PRODUCTION_onMeasure */
}

fun main() {
  val rendered = RenderedDocument()
  val view = AssignmentView()
  var latest: TextSegment? = null
  repeat(100) { revision ->
    val content = Spanned("x".repeat(revision + 1))
    val blocks = listOf(AccessibleBlock(0, content.length, heading = true, taskState = 2))
    val details = listOf(DetailsBlock(0, content.length, true, listOf(Container(CONTAINER_DETAILS, revision))))
    val segment = TextSegment(content, blocks, details)
    view.setSegment(segment, rendered, 7)
    check(view.assignments == revision + 1) { "canonical revision must assign text once" }
    check(view.readingBuilds == revision + 1) { "canonical revision must build readings once; got ${view.readingBuilds}" }
    check(view.accessibility.notifications == revision + 1) { "canonical revision must notify once" }
    check(view.lastBlocks === blocks && view.lastDetails === details)
    view.checkSegment(segment, 7)
    view.checkDetails(details.single())
    view.checkReadingRange(content.length)
    latest = segment
  }
  check(DownstreamNative.events.size == 200)
  repeat(100) { view.setSegment(latest!!, rendered, 7) }
  check(view.assignments == 100 && view.readingBuilds == 100 && view.accessibility.notifications == 100)
  view.setSegment(latest!!, rendered, 8)
  check(view.assignments == 101 && view.readingBuilds == 101 && view.accessibility.notifications == 101)
  view.checkSegment(latest, 8)

  val body = Spanned("code body")
  val bodyBlocks = listOf(AccessibleBlock(0, body.length))
  view.setContent(body, bodyBlocks, rendered)
  check(view.text === body && view.lastBlocks === bodyBlocks && view.lastDetails.isEmpty())
  check(view.assignments == 102 && view.readingBuilds == 102 && view.accessibility.notifications == 102)
  view.checkSegment(null, 8)
  view.checkDetails(null)
  view.checkStreaming(null, false)
  check(view.highlightColor == 17 && view.buffer == BufferType.NORMAL)
  check(view.paint.typeface == rendered.paint.typeface && view.paint.textSize == rendered.paint.textSize &&
        view.paint.color == rendered.paint.color)
  view.setSelectable(true)
  view.setContent(body, bodyBlocks, rendered)
  check(view.buffer == BufferType.SPANNABLE)
  PrecomputedTexts.enabled = true
  view.setContent(body, bodyBlocks, rendered)
  check(view.assignments == 104 && view.readingBuilds == 104 && view.accessibility.notifications == 104)
  PrecomputedTexts.enabled = false

  // Growing live revisions keep the canonical TextView empty and expose each
  // segment's text to the actual reading-order builder.
  val liveView = AssignmentView()
  liveView.width = 200
  val model = StreamingParagraph()
  var live: TextSegment? = null
  repeat(100) { revision ->
    val content = Spanned("y".repeat(revision + 1))
    live = TextSegment(content, listOf(AccessibleBlock(0, content.length)), streamingParagraph = model)
    liveView.setSegment(live!!, rendered, 4)
    liveView.checkSegment(live, 4)
    liveView.checkStreaming(rendered, false)
    liveView.checkReadingRange(content.length)
    check(liveView.text.isEmpty() && liveView.assignments == 0)
    check(liveView.readingBuilds == revision + 1 && liveView.accessibility.notifications == revision + 1)
  }
  liveView.setSegment(live!!, rendered, 4)
  check(liveView.readingBuilds == 100 && liveView.accessibility.notifications == 100)
  model.warm = false
  liveView.onMeasure(200, 400)
  check(liveView.text === live!!.text)
  liveView.checkSegment(live, 4)
  liveView.checkStreaming(rendered, true)
  check(liveView.assignments == 1 && liveView.readingBuilds == 101 && liveView.accessibility.notifications == 101)
  liveView.onMeasure(200, 400)
  check(liveView.assignments == 1 && liveView.readingBuilds == 101 && liveView.accessibility.notifications == 101)
  model.warm = true
  liveView.onMeasure(200, 400)
  liveView.checkStreaming(rendered, false)
  check(liveView.text.isEmpty() && liveView.assignments == 2)
  liveView.setSelectable(true)
  liveView.checkSegment(live, 4)
  liveView.checkStreaming(rendered, true)
  check(liveView.text === live!!.text && liveView.isTextSelectable)
  check(liveView.assignments == 3 && liveView.readingBuilds == 102 && liveView.accessibility.notifications == 102)
  val settled = TextSegment(Spanned("settled"), listOf(AccessibleBlock(0, 7)))
  liveView.setSegment(settled, rendered, 4)
  liveView.checkSegment(settled, 4)
  liveView.checkStreaming(null, false)
  check(liveView.assignments == 4 && liveView.readingBuilds == 103 && liveView.accessibility.notifications == 103)
  liveView.setSegment(settled, rendered, 4)
  check(liveView.assignments == 4 && liveView.readingBuilds == 103 && liveView.accessibility.notifications == 103)
  println("text assignment: 100 canonical revisions assign/build/notify once; identity, details, direct content and live fallback preserve state")
}
