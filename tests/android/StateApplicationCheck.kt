package com.downstream

// Host stand-ins record calls made by production state methods. Segment
// reconciliation/drawing, the Android event loop, and JNI are not executed.
internal class StateWrapper
internal object PixelUtil {
  fun toPixelFromDIP(value: Double): Double = value * 2
}
internal object DownstreamNative {
  val traces = mutableListOf<Pair<Long, String>>()
  fun isTracingDocument(id: Long): Boolean = true
  fun traceDocument(id: Long, name: String) { traces.add(id to name) }
}
internal class RenderedDocument(val id: Long, val scale: Float, val theme: Long, val caret: PlacedCaret) {
  companion object {
    val obtains = mutableListOf<RenderedDocument>()
    fun obtain(context: Any, id: Long, scale: Float, syntax: Boolean, theme: Long, caret: PlacedCaret): RenderedDocument =
      RenderedDocument(id, scale, theme, caret).also { obtains.add(it) }
  }
}
internal data class MountedState(
  val id: Long, val code: CodeLayout, val images: ImageLayout,
  val hosts: HostLayout, val chrome: Int, val caret: PlacedCaret,
)
internal class StateHost {
  private val context = Any()
  private var documentId = 0L
  private var fontScale = 1f
  private var themeId = 0L
  private var rendered: RenderedDocument? = null
  private var syntaxHighlighting = true
  private var codeLayout = CodeLayout()
  private var imageLayout = ImageLayout()
  private var hostLayout = HostLayout()
  private var chrome = 0
  private var caret = PlacedCaret.NONE
  private var pendingTraceDocumentId = 0L
  var stateWrapper: StateWrapper? = null
  var propTheme = 0L
  val mounted = mutableListOf<MountedState>()
  val revealed = mutableListOf<Long>()
  var layoutRequests = 0
  var hostLayoutRequests = 0
  var frameRequests = 0
  private fun internProps(): Long = propTheme
  private fun syncSegmentViews() {
    val document = rendered
    check(document == null || document.id == documentId && document.caret == caret)
    mounted.add(MountedState(documentId, codeLayout, imageLayout, hostLayout, chrome, caret))
  }
  private fun applyReveal() { revealed.add(documentId) }
  private fun scheduleTraceFrame() { frameRequests++ }
  private fun applySettledState() {}
  private fun requestLayout() { layoutRequests++ }
  private fun layoutHostChildren() { hostLayoutRequests++ }
  fun clearCounts() {
    mounted.clear()
    revealed.clear()
    RenderedDocument.obtains.clear()
    DownstreamNative.traces.clear()
    layoutRequests = 0
    hostLayoutRequests = 0
    frameRequests = 0
  }
  // reset's document/render/caret fields, after reveal disposal.
  fun recycle() {
    documentId = 0
    rendered = null
    themeId = 0
    caret = PlacedCaret.NONE
    codeLayout = CodeLayout()
    pendingTraceDocumentId = 0
  }

  /* PRODUCTION_STATE_METHODS */
}

fun main() {
  val code = CodeLayout(CodeLayoutProps(true, false, 500f), CodeChoices(intArrayOf(0), intArrayOf(1)))
  val images = ImageLayout(ImageStateLayout(entries = listOf(ImageEntry(0, ImageState.LOADED, 120f, 80f))))
  val hosts = HostLayout(listOf(HostEntry(0, 180f)))
  val caret = PlacedCaret(1, 1)
  val host = StateHost()
  host.setDocument(1, 1f, 5)
  host.clearCounts()

  // Previous Fabric order, using the unchanged production standalone setters.
  host.setCodeProps(true, false, 250.0)
  host.setCodeChoices(intArrayOf(0), intArrayOf(1))
  host.setImageLayout(images)
  host.setHostLayout(hosts)
  host.setChrome(3)
  host.setCaret(caret)
  host.setDocument(2, 1.5f, 6)
  check(host.mounted.size == 4)
  check(RenderedDocument.obtains.map { it.id } == listOf(1L, 2L))
  val oldSyncs = host.mounted.size
  val oldObtains = RenderedDocument.obtains.size

  val atomic = StateHost()
  atomic.setDocument(1, 1f, 5)
  atomic.clearCounts()
  val wrapper = StateWrapper()
  atomic.applyState(wrapper, 2, 1.5f, 6, code, images, hosts, 3, caret)
  val expected = MountedState(2, code, images, hosts, 3, caret)
  check(atomic.mounted == listOf(expected))
  check(RenderedDocument.obtains.size == 1)
  check(RenderedDocument.obtains.single().let { it.id == 2L && it.scale == 1.5f && it.theme == 6L && it.caret == caret })
  check(atomic.revealed == listOf(2L))
  check(atomic.hostLayoutRequests == 1 && atomic.layoutRequests == 1)
  check(DownstreamNative.traces == listOf(1L to "frame_unavailable", 2L to "apply_start", 2L to "apply_end"))
  check(atomic.frameRequests == 1)
  println("ok Fabric state work: sync $oldSyncs -> ${atomic.mounted.size}, obtain $oldObtains -> ${RenderedDocument.obtains.size}")

  // A repeated snapshot must still replace its wrapper for future interactions.
  atomic.clearCounts()
  val rebound = StateWrapper()
  atomic.applyState(rebound, 2, 1.5f, 6, code, images, hosts, 3, caret)
  check(atomic.stateWrapper === rebound)
  check(atomic.mounted.isEmpty() && RenderedDocument.obtains.isEmpty() && atomic.revealed.isEmpty())

  // Each option independently invalidates a same-document mount.
  val optionCases = listOf(
    MountedState(2, code, ImageLayout(), HostLayout(), 0, PlacedCaret.NONE),
    MountedState(2, CodeLayout(), images, HostLayout(), 0, PlacedCaret.NONE),
    MountedState(2, CodeLayout(), ImageLayout(), hosts, 0, PlacedCaret.NONE),
    MountedState(2, CodeLayout(), ImageLayout(), HostLayout(), 3, PlacedCaret.NONE),
  )
  for (expectedOption in optionCases) {
    val changing = StateHost()
    changing.setDocument(2, 1.5f, 6)
    changing.clearCounts()
    changing.applyState(wrapper, 2, 1.5f, 6, expectedOption.code, expectedOption.images,
      expectedOption.hosts, expectedOption.chrome, expectedOption.caret)
    check(changing.mounted == listOf(expectedOption))
    check(RenderedDocument.obtains.isEmpty() && changing.revealed.isEmpty())
  }

  // Same document, changed geometry: reuse text, sync once under final options.
  atomic.clearCounts()
  atomic.applyState(wrapper, 2, 1.5f, 6, CodeLayout(), ImageLayout(), HostLayout(), 0, caret)
  check(atomic.mounted.single() == MountedState(2, CodeLayout(), ImageLayout(), HostLayout(), 0, caret))
  check(RenderedDocument.obtains.isEmpty() && atomic.revealed.isEmpty())
  check(atomic.layoutRequests == 1 && atomic.hostLayoutRequests == 1)

  // A caret-only change rebuilds this document without reapplying its reveal.
  atomic.clearCounts()
  atomic.applyState(wrapper, 2, 1.5f, 6, CodeLayout(), ImageLayout(), HostLayout(), 0, PlacedCaret.NONE)
  check(RenderedDocument.obtains.single().let { it.id == 2L && it.caret == PlacedCaret.NONE })
  check(atomic.mounted.size == 1 && atomic.revealed.isEmpty())

  // Theme fallback and empty/recycled mounts still take the document path.
  atomic.clearCounts()
  atomic.propTheme = 9
  atomic.applyState(wrapper, 2, 1.5f, 0, CodeLayout(), ImageLayout(), HostLayout(), 0, PlacedCaret.NONE)
  check(RenderedDocument.obtains.single().theme == 9L && atomic.revealed == listOf(2L))
  atomic.clearCounts()
  atomic.applyState(wrapper, 0, 1f, 0, CodeLayout(), ImageLayout(), HostLayout(), 0, PlacedCaret.NONE)
  check(RenderedDocument.obtains.isEmpty() && atomic.mounted.single().id == 0L)
  atomic.recycle()
  atomic.clearCounts()
  atomic.applyState(wrapper, 2, 1.5f, 6, code, images, hosts, 3, caret)
  check(atomic.mounted == listOf(expected) && RenderedDocument.obtains.single().id == 2L)
  println("ok Fabric state same-document changes, no-op, tracing, theme fallback and remount")
}
