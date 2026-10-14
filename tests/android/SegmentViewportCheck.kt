package com.downstream

import java.util.ArrayDeque
import java.util.WeakHashMap

private class Context {
  val accessibility = AccessibilityManager()
  fun getSystemService(key: String) = accessibility
  companion object { const val ACCESSIBILITY_SERVICE = "accessibility" }
}
private class AccessibilityManager { var isEnabled = false }
private class Metrics { val density = 1f }
private class Resources { val displayMetrics = Metrics() }
private class Rect(var left: Int = 0, var top: Int = 0, var right: Int = 0, var bottom: Int = 0) {
  val isEmpty get() = left >= right || top >= bottom
  fun height() = bottom - top
  fun setEmpty() { left = 0; top = 0; right = 0; bottom = 0 }
  fun set(other: Rect) { left = other.left; top = other.top; right = other.right; bottom = other.bottom }
  override fun equals(other: Any?) = other is Rect && left == other.left && top == other.top && right == other.right && bottom == other.bottom
  override fun hashCode() = top
}
private class ViewTreeObserver {
  fun interface OnPreDrawListener { fun onPreDraw(): Boolean }
  var isAlive = true
  val listeners = mutableListOf<OnPreDrawListener>()
  fun addOnPreDrawListener(listener: OnPreDrawListener) { listeners.add(listener) }
  fun removeOnPreDrawListener(listener: OnPreDrawListener) { listeners.remove(listener) }
  fun draw() { listeners.toList().forEach { check(it.onPreDraw()) } }
}
private open class View(val context: Context) {
  var measuredWidth = 0; var measuredHeight = 0
  var left = 0; var top = 0; var right = 0; var bottom = 0
  var isLayoutRequested = true
  var visibility = VISIBLE
  var measures = 0; var layouts = 0
  fun requestLayout() { isLayoutRequested=true }
  fun measure(width: Int, height: Int) { measures++; measuredWidth = width; measuredHeight = height }
  fun layout(l: Int, t: Int, r: Int, b: Int) { layouts++; left=l; top=t; right=r; bottom=b; isLayoutRequested=false }
  companion object { const val VISIBLE=0; const val INVISIBLE=4; const val GONE=8 }
}
private class Layer {
  var top=0
  val children = mutableListOf<View>()
  fun removeView(view: View) { children.remove(view) }
  fun addView(view: View) { children.add(view) }
}
private object MeasureSpec { const val EXACTLY=1; fun makeMeasureSpec(size: Int, mode: Int)=size }
private class LinkSpan
private class TagSpan
private class SpoilerSpan
private class MathInlineSpan
private class Text(val value: String, val spans: List<Any> = emptyList()) {
  val length get()=value.length
  fun <T> getSpans(start: Int, end: Int, klass: Class<T>): Array<T> {
    val values=spans.filter { klass.isInstance(it) }
    @Suppress("UNCHECKED_CAST")
    return java.lang.reflect.Array.newInstance(klass, values.size).let { out -> values.forEachIndexed { index, value -> java.lang.reflect.Array.set(out,index,value) }; out as Array<T> }
  }
}
private sealed class Segment { var detailsContainers: List<Int> = emptyList() }
private class TextSegment(val text: Text, val customContainer: Int=-1, val caretOffset: Int=-1,
  detailsContainers: List<Int> = emptyList(), val detailsBlocks: List<Int> = emptyList(), val taskBlocks: List<Int> = emptyList()): Segment() { init { this.detailsContainers = detailsContainers } }
private class CodeSegment(val codeOrdinal: Int=0, val firstLine: String="line-$codeOrdinal", val caretOffset: Int=-1): Segment()
private class TableSegment(val ordinal: Int=0, val rtl: Boolean=false, val accessibilityRows: List<String> = listOf("header-$ordinal"), val caretKind:Int=0, val tableWidth: Int=1000, val identity: Long=0): Segment()
private class ImageSegment: Segment()
private class MathSegment(val literal: String="formula"): Segment()
private class Plan(val from: Int, val to: Int)
private class Style { val chromeRowHeight=20; val imageMetrics=0; val imageAltColor=0; val mathFontSize=0; val mathColor=0; val mathBackground=0; val mathErrorColor=0; val mathPadding=0; val mathRadius=0 }
private class RenderedDocument(input: List<Segment>, val positions: IntArray, val tracing: Boolean = false, val documentId: Long = 42L) {
  var segmentReads=0
  val segments: List<Segment> = object : AbstractList<Segment>() {
    override val size get()=input.size
    override fun get(index:Int):Segment { segmentReads++; return input[index] }
  }
  val plan=segments.indices.map { Plan(it,it+1) }; val style=Style()
  fun frames(width: Int, chrome: Int, code: Int, images: Int, hosts: Hosts, tables: Int, choices: Int)=positions
}
private object DownstreamNative {
  val events = mutableListOf<Pair<Long, String>>()
  fun traceDocument(documentId: Long, event: String) { events.add(documentId to event) }
}
private class TextSegmentView(context: Context): View(context) {
  var shown: TextSegment?=null
  init { creations++ }
  fun setSegment(segment: TextSegment, document: RenderedDocument, from: Int) {
    if (shown === segment) return
    bindings++; shown=segment; isLayoutRequested=true
  }
  fun clearContent() { shown=null }
  companion object { var creations=0; var bindings=0 }
}
private class Scroller { var scrollX=0; var scrollY=0; var width=200; var pin=1
  fun capturePinState()=pin
  fun restorePinState(value:Int) { pin=value }
}
private class CodeSegmentView(context: Context): View(context) {
  var shown: CodeSegment?=null
  private val scroll=Scroller(); private val viewport=Scroller()
  private var savedOffset=0; private var savedVerticalOffset=0; private var restoreScroll=false
  var state: IntArray get()=captureViewportState(); set(value) { restoreViewportState(value) }
  init { creations++ }
  fun setControlHost(host: MountedDocument) {}; fun setHosted(value: Boolean) {}
  fun setSegment(s:CodeSegment,d:RenderedDocument,f:Int) { if(shown !== s) isLayoutRequested=true; shown=s }
  /* PRODUCTION_CODE_SCROLL */
  fun clearViewportContent() { shown=null; restoreScroll=false; scroll.scrollX=0; viewport.scrollY=0; viewport.pin=1 }
  companion object { var creations=0 }
}
private class TableSegmentView(context: Context): View(context) {
  var shown: TableSegment?=null
  private val segment get()=shown
  private val scroll=Scroller(); private val viewport=Scroller()
  private var pendingLeadingDistance:Int?=null; private var pendingVerticalOffset:Int?=null
  var state: IntArray get()=captureViewportState(); set(value) { restoreViewportState(value) }
  init { creations++ }
  fun setControlHost(host: MountedDocument) {}
  fun setSegment(s:TableSegment,e:Int,h:Int) { if(shown !== s) isLayoutRequested=true; shown=s }
  /* PRODUCTION_TABLE_SCROLL */
  fun clearViewportContent() { shown=null; pendingLeadingDistance=0; pendingVerticalOffset=0; scroll.scrollX=0; viewport.scrollY=0; viewport.pin=1 }
  companion object { var creations=0 }
}
private class ImageSegmentView(context: Context): View(context) {
  var onImageAnswer: ((String, Int)->Unit)?=null; var onImagePress: ((String,String,String,Int)->Unit)?=null; var onImageDownload: ((Int)->Unit)?=null
  fun setStyle(a:Int,b:Int) {}; fun setStatusLabels(a:String,b:String) {}; fun setImageLayout(a:Int) {}; fun setSegment(s:ImageSegment) {}
}
private class MathSegmentView(context: Context): View(context) {
  var shown: MathSegment?=null
  private val scroll=Scroller(); private var pendingScrollOffset:Int?=null
  var state: IntArray get()=captureViewportState(); set(value) { restoreViewportState(value) }
  init { creations++ }
  fun setStyle(a:Int,b:Int,c:Int,d:Int,e:Int,f:Int) {}
  fun setSegment(s:MathSegment) { if(shown !== s) isLayoutRequested=true; shown=s }
  /* PRODUCTION_MATH_SCROLL */
  fun clearViewportContent() { shown=null; pendingScrollOffset=0; scroll.scrollX=0 }
  companion object { var creations=0 }
}
private class Hosts { val ordinals=mutableSetOf<Int>(); fun hosts(index:Int)=index in ordinals }
private class Targets { fun build(ranges:LongArray,ends:IntArray) {} }
private const val LABEL_IMAGE_NOT_AVAILABLE=0
private const val LABEL_IMAGE_BLOCKED=1
private class SegmentSlots: ArrayList<View>() {
  var shifts=0
  override fun add(index:Int, element:View) { if(index < size) shifts++; super.add(index,element) }
  override fun removeAt(index:Int):View { if(index < size-1) shifts++; return super.removeAt(index) }
}
private class MountedDocument(val context: Context, var rendered: RenderedDocument?) {
  val resources=Resources(); val segmentLayer=Layer(); val segmentViews=SegmentSlots()
  val recycledTextViews=ArrayDeque<TextSegmentView>(); val segmentEligibility=WeakHashMap<TextSegment,Boolean>()
  private class DeferredSegmentView(context:Context): View(context)
  var width=200; var height=200000; var paddingLeft=0; var paddingRight=0; var paddingTop=0
  var selectable=false; var isAttachedToWindow=true
  val segmentViewport=Rect(); var segmentViewportKnown=false; var segmentAccessibilityEnabled=false
  var actualVisible: Rect?=Rect(0,0,200,100)
  val viewTreeObserver=ViewTreeObserver(); var segmentViewportObserver: ViewTreeObserver?=null
  var viewportOnlyIndices: IntArray?=null
  val recycledCodeViews=ArrayDeque<CodeSegmentView>(); val recycledTableViews=ArrayDeque<TableSegmentView>(); val recycledMathViews=ArrayDeque<MathSegmentView>()
  private data class SegmentScrollState(val tableIdentity:Long, val viewport:IntArray)
  private val segmentScrollStates=mutableMapOf<String,SegmentScrollState>(); var segmentScrollEpoch = -1
  var realizedDocument: RenderedDocument?=null
  var virtualizedDocument: RenderedDocument?=null
  var virtualizedFrames: IntArray?=null
  var virtualizedInterval=0 until 0
  var checkedFrameOrder: IntArray?=null
  var virtualizedFramesOrdered=false
  var revealSegmentEnds=IntArray(0); val revealRanges=LongArray(0); val revealTargets=Targets()
  val chrome=0; val codeLayout=0; val imageLayout=0; val hostLayout=Hosts(); val tableLayout=0; val readerChoices=0
  var sourceEpoch=0; val controlLabels=listOf("none","blocked")
  fun reportImageAnswer(url:String, answer:Int) {}; fun pressImage(url:String,alt:String,title:String,range:Int) {}; fun downloadImage(image:Int) {}
  fun applySettledState() {}
  fun getLocalVisibleRect(out:Rect):Boolean { val rect=actualVisible ?: return false; out.set(rect); return true }
  fun drive()=syncSegmentViews()
  fun attach()=observeSegmentViewport()
  /* PRODUCTION_METHODS */
}
fun main() {
  val segments=List(10_000) { TextSegment(Text("original-$it")) }
  val positions=IntArray(20_000) { if(it%2==0) it/2*20 else 20 }
  val context=Context(); val host=MountedDocument(context,RenderedDocument(segments,positions))
  host.attach()
  check(host.segmentLayer.children.size <= 10)
  check(host.segmentViews.size==10_000)
  val initial=TextSegmentView.creations
  host.viewTreeObserver.draw()
  check(TextSegmentView.creations==initial) { "unchanged viewport allocated text views" }
  val scrolls=listOf(2000,4000,198000,0)
  for(top in scrolls) {
    val before=host.rendered!!.segmentReads
    host.actualVisible=Rect(0,top,200,top+100); host.viewTreeObserver.draw()
    check(host.rendered!!.segmentReads-before < 100) { "scroll traversed offscreen segment slots" }
    check(host.segmentLayer.children.size <= 13) { "offscreen text views remained mounted" }
    for(child in host.segmentLayer.children) {
      val text=child as TextSegmentView
      val index=text.top/20
      check(text.shown === segments[index]) { "recycled text view displays the wrong segment" }
    }
  }
  check(TextSegmentView.creations < 30) { "scroll did not reuse text views" }
  host.actualVisible=null; host.viewTreeObserver.draw()
  check(host.segmentLayer.children.isEmpty()) { "fully clipped document kept text mounted" }
  host.actualVisible=Rect(0,0,200,100); host.viewTreeObserver.draw()
  check(host.segmentLayer.children.isNotEmpty())
  val revised=segments.toMutableList(); revised[0]=TextSegment(Text("newest revision"))
  host.rendered=RenderedDocument(revised,positions); host.drive()
  check((host.segmentLayer.children.first { it.top==0 } as TextSegmentView).shown===revised[0])
  check(segments[0].text.value=="original-0") { "view recycling mutated a source snapshot" }
  context.accessibility.isEnabled=true; host.viewTreeObserver.draw()
  check(host.segmentLayer.children.size==10_000) { "accessibility lost offscreen reading order" }
  context.accessibility.isEnabled=false; host.viewTreeObserver.draw()
  check(host.segmentLayer.children.size <= 10)
  host.selectable=true; host.drive()
  check(host.segmentLayer.children.size==10_000) { "selectable text must keep every text view" }
  host.selectable=false; host.drive()
  host.isAttachedToWindow=false; host.viewTreeObserver.draw()
  check(host.segmentLayer.children.size==10_000) { "unknown viewport must preserve mounted fallback" }
  val protected=listOf(TextSegment(Text("spoiler",listOf(SpoilerSpan()))),TextSegment(Text("caret"),caretOffset=0),
    TextSegment(Text("host"),customContainer=3),TextSegment(Text("details"),detailsBlocks=listOf(1)),
    CodeSegment(caretOffset=0),TableSegment(caretKind=1),ImageSegment(),MathSegment().apply { detailsContainers=listOf(1) })
  check(DownstreamNative.events.isEmpty()) { "disabled diagnostics emitted table events" }
  val protectedHost=MountedDocument(Context(),RenderedDocument(protected,IntArray(protected.size*2) { if(it%2==0) 10000+it*20 else 20 }, tracing = true))
  protectedHost.attach()
  check(protectedHost.segmentLayer.children.size==protected.size) { "stateful or hosted segments must not be deferred" }
  check(DownstreamNative.events == listOf(
    42L to "android_table_create_start", 42L to "android_table_create_end",
    42L to "android_table_bind_start", 42L to "android_table_bind_end",
  )) { "table diagnostics lost document identity or balanced stages" }
  DownstreamNative.events.clear()
  protectedHost.drive()
  check(DownstreamNative.events == listOf(42L to "android_table_bind_start", 42L to "android_table_bind_end")) {
    "reused table repeated creation diagnostics"
  }
  val paddedHost=MountedDocument(Context(),RenderedDocument(segments,positions))
  paddedHost.paddingTop=100; paddedHost.segmentLayer.top=100
  paddedHost.actualVisible=Rect(0,200,200,300); paddedHost.attach()
  check(paddedHost.segmentLayer.children.any { it.top==100 && (it as TextSegmentView).shown===segments[0] }) {
    "ancestor viewport and segment layer padding must use the same coordinate space"
  }
  val overlapFrames=intArrayOf(0,20,10000,20,10,20,9000,20)
  val overlapSegments=List(4) { TextSegment(Text("overlap-$it")) }
  val overlapHost=MountedDocument(Context(),RenderedDocument(overlapSegments,overlapFrames))
  overlapHost.actualVisible=Rect(0,0,200,50); overlapHost.attach()
  check(overlapHost.segmentLayer.children.size==2)
  overlapHost.actualVisible=Rect(0,5,200,55); overlapHost.viewTreeObserver.draw()
  check(overlapHost.segmentLayer.children.size==2 && !overlapHost.virtualizedFramesOrdered) {
    "nonmonotonic frames must preserve visible overlaps through the full fallback"
  }
  val mixed = List(10_000) { index -> when(index % 4) {
    0 -> TextSegment(Text("formatted-$index", listOf(LinkSpan(), TagSpan(), MathInlineSpan())), taskBlocks=listOf(index))
    1 -> CodeSegment(index)
    2 -> TableSegment(index, rtl=index % 8==2)
    else -> MathSegment("formula-$index")
  } }
  val mixedHost=MountedDocument(Context(),RenderedDocument(mixed,positions))
  mixedHost.attach()
  val code=mixedHost.segmentViews[1] as CodeSegmentView
  val table=mixedHost.segmentViews[2] as TableSegmentView
  val math=mixedHost.segmentViews[3] as MathSegmentView
  code.state=intArrayOf(70,35,0); table.state=intArrayOf(120,50,0); math.state=intArrayOf(90)
  val creationsBefore=intArrayOf(TextSegmentView.creations,CodeSegmentView.creations,TableSegmentView.creations,MathSegmentView.creations)
  for(top in listOf(2000,4000,198000,0)) {
    val before=mixedHost.rendered!!.segmentReads
    mixedHost.actualVisible=Rect(0,top,200,top+100); mixedHost.viewTreeObserver.draw()
    check(mixedHost.rendered!!.segmentReads-before < 120) { "mixed scroll traversed offscreen slots" }
    check(mixedHost.segmentLayer.children.size <= 13) { "mixed offscreen native views remained mounted" }
    for(child in mixedHost.segmentLayer.children) {
      val index=child.top/20
      val shown=when(child) {
        is TextSegmentView -> child.shown
        is CodeSegmentView -> child.shown
        is TableSegmentView -> child.shown
        is MathSegmentView -> child.shown
        else -> error("unknown mounted mixed view")
      }
      check(shown === mixed[index]) { "recycled mixed view displays wrong snapshot" }
      check(child.measuredWidth==200 && child.measuredHeight==20 && child.bottom==child.top+20) { "mixed realization changed canonical frames" }
    }
  }
  val created=intArrayOf(TextSegmentView.creations,CodeSegmentView.creations,TableSegmentView.creations,MathSegmentView.creations)
  check(created.indices.all { created[it]-creationsBefore[it] < 12 }) { "mixed scroll did not reuse bounded view pools" }
  check((mixedHost.segmentViews[1] as CodeSegmentView).state.contentEquals(intArrayOf(70,35,0))) { "code scroll or reader unpin lost on remount" }
  check((mixedHost.segmentViews[2] as TableSegmentView).state.contentEquals(intArrayOf(120,50,0))) { "RTL table leading distance, vertical scroll or reader unpin lost" }
  check((mixedHost.segmentViews[3] as MathSegmentView).state.contentEquals(intArrayOf(90))) { "display math horizontal scroll lost" }
  check(mixedHost.segmentViews.shifts==0) { "realization shifted the document-sized slot array" }
  val unchangedLayouts=mixedHost.segmentLayer.children.sumOf { it.layouts }
  mixedHost.drive()
  check(mixedHost.segmentLayer.children.sumOf { it.layouts }==unchangedLayouts) { "unchanged mixed snapshot laid out views again" }
  mixedHost.actualVisible=null; mixedHost.viewTreeObserver.draw()
  val appended=mixed.toMutableList()
  appended[1]=CodeSegment(1); appended[2]=TableSegment(2,rtl=true)
  mixedHost.rendered=RenderedDocument(appended,positions); mixedHost.drive()
  mixedHost.actualVisible=Rect(0,0,200,100); mixedHost.viewTreeObserver.draw()
  check((mixedHost.segmentViews[1] as CodeSegmentView).state[0]==70) { "growing code lost reader offset" }
  check((mixedHost.segmentViews[2] as TableSegmentView).state[0]==120) { "growing table lost leading distance" }
  mixedHost.actualVisible=null; mixedHost.viewTreeObserver.draw()
  mixedHost.sourceEpoch++; mixedHost.rendered=RenderedDocument(mixed,positions); mixedHost.drive()
  mixedHost.actualVisible=Rect(0,0,200,100); mixedHost.viewTreeObserver.draw()
  check((mixedHost.segmentViews[1] as CodeSegmentView).state.contentEquals(intArrayOf(0,0,1))) { "new source inherited code scroll state" }
  check((mixedHost.segmentViews[2] as TableSegmentView).state.contentEquals(intArrayOf(0,0,1))) { "new source inherited table scroll state" }
  mixedHost.context.accessibility.isEnabled=true; mixedHost.viewTreeObserver.draw()
  check(mixedHost.segmentLayer.children.size==10_000) { "mixed accessibility fallback lost reading order" }
  mixedHost.context.accessibility.isEnabled=false; mixedHost.viewTreeObserver.draw()
  check(mixedHost.segmentLayer.children.size <= 10)
  mixedHost.selectable=true; mixedHost.drive()
  check(mixedHost.segmentLayer.children.size==10_000) { "mixed selectable fallback lost native content" }
  val hosted=MountedDocument(Context(),RenderedDocument(listOf(CodeSegment(12)),intArrayOf(10000,20)))
  hosted.hostLayout.ordinals.add(12); hosted.attach()
  check(hosted.segmentLayer.children.size==1) { "hosted code lost native container chrome" }
  val replacementHost=MountedDocument(Context(),RenderedDocument(listOf(TableSegment(0, identity=11)),intArrayOf(0,20)))
  replacementHost.attach()
  (replacementHost.segmentViews[0] as TableSegmentView).state=intArrayOf(95,10,0)
  replacementHost.actualVisible=null; replacementHost.viewTreeObserver.draw()
  replacementHost.rendered=RenderedDocument(listOf(TableSegment(0, identity=22)),intArrayOf(0,20)); replacementHost.drive()
  replacementHost.actualVisible=Rect(0,0,200,100); replacementHost.viewTreeObserver.draw()
  check((replacementHost.segmentViews[0] as TableSegmentView).state.contentEquals(intArrayOf(0,0,1))) { "different table identity inherited stale scroll state" }
  println("android segment viewport: 10000 plain and mixed slots <=13 mounted views, <120 segment reads per scroll; bounded reuse, exact geometry, latest snapshots, code/table/math scroll + pin retention, source reset, selection/accessibility and stateful fallback passed")
}
