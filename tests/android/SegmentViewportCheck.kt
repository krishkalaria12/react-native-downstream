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
private sealed class Segment
private class TextSegment(val text: Text, val customContainer: Int=-1, val caretOffset: Int=-1,
  val detailsContainers: List<Int> = emptyList(), val detailsBlocks: List<Int> = emptyList(), val taskBlocks: List<Int> = emptyList()): Segment()
private class CodeSegment(val codeOrdinal: Int=0): Segment()
private class TableSegment: Segment()
private class ImageSegment: Segment()
private class MathSegment: Segment()
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
private class CodeSegmentView(context: Context): View(context) { fun setControlHost(host: MountedDocument) {}; fun setHosted(value: Boolean) {}; fun setSegment(s:CodeSegment,d:RenderedDocument,f:Int) {} }
private class TableSegmentView(context: Context): View(context) { fun setControlHost(host: MountedDocument) {}; fun setSegment(s:TableSegment,e:Int,h:Int) {} }
private class ImageSegmentView(context: Context): View(context) {
  var onImageAnswer: ((String, Int)->Unit)?=null; var onImagePress: ((String,String,String,Int)->Unit)?=null; var onImageDownload: ((Int)->Unit)?=null
  fun setStyle(a:Int,b:Int) {}; fun setStatusLabels(a:String,b:String) {}; fun setImageLayout(a:Int) {}; fun setSegment(s:ImageSegment) {}
}
private class MathSegmentView(context: Context): View(context) { fun setStyle(a:Int,b:Int,c:Int,d:Int,e:Int,f:Int) {}; fun setSegment(s:MathSegment) {} }
private class Hosts { fun hosts(index:Int)=false }
private class Targets { fun build(ranges:LongArray,ends:IntArray) {} }
private const val LABEL_IMAGE_NOT_AVAILABLE=0
private const val LABEL_IMAGE_BLOCKED=1
private class MountedDocument(val context: Context, var rendered: RenderedDocument?) {
  val resources=Resources(); val segmentLayer=Layer(); val segmentViews=mutableListOf<View>()
  val recycledTextViews=ArrayDeque<TextSegmentView>(); val segmentEligibility=WeakHashMap<TextSegment,Boolean>()
  private class DeferredTextView(context:Context): View(context)
  var width=200; var height=200000; var paddingLeft=0; var paddingRight=0; var paddingTop=0
  var selectable=false; var isAttachedToWindow=true
  val segmentViewport=Rect(); var segmentViewportKnown=false; var segmentAccessibilityEnabled=false
  var actualVisible: Rect?=Rect(0,0,200,100)
  val viewTreeObserver=ViewTreeObserver(); var segmentViewportObserver: ViewTreeObserver?=null
  var viewportOnlyIndices: IntArray?=null
  var virtualizedDocument: RenderedDocument?=null
  var virtualizedFrames: IntArray?=null
  var virtualizedInterval=0 until 0
  var checkedFrameOrder: IntArray?=null
  var virtualizedFramesOrdered=false
  var revealSegmentEnds=IntArray(0); val revealRanges=LongArray(0); val revealTargets=Targets()
  val chrome=0; val codeLayout=0; val imageLayout=0; val hostLayout=Hosts(); val tableLayout=0; val readerChoices=0
  val sourceEpoch=0; val controlLabels=listOf("none","blocked")
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
  val protected=listOf(TextSegment(Text("link",listOf(LinkSpan()))),TextSegment(Text("tag",listOf(TagSpan()))),
    TextSegment(Text("spoiler",listOf(SpoilerSpan()))),TextSegment(Text("math",listOf(MathInlineSpan()))),TextSegment(Text("caret"),caretOffset=0),
    TextSegment(Text("host"),customContainer=3),TextSegment(Text("details"),detailsBlocks=listOf(1)),
    TextSegment(Text("task"),taskBlocks=listOf(1)),CodeSegment(),TableSegment(),ImageSegment(),MathSegment())
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
  println("android segment viewport: 10000 plain slots <=13 mounted text views, <100 segment reads per scroll; recycling, latest revision, selection, accessibility, hidden/re-exposure and interactive fallback passed")
}
