package com.downstream

import android.content.Context
import com.facebook.react.bridge.Arguments
import com.facebook.react.bridge.ReactContext
import com.facebook.react.bridge.ReadableArray
import com.facebook.react.bridge.ReadableMap
import com.facebook.react.bridge.WritableMap
import com.facebook.react.module.annotations.ReactModule
import com.facebook.react.uimanager.PixelUtil
import com.facebook.react.uimanager.ReactStylesDiffMap
import com.facebook.react.uimanager.SimpleViewManager
import com.facebook.react.uimanager.StateWrapper
import com.facebook.react.uimanager.ThemedReactContext
import com.facebook.react.uimanager.UIManagerHelper
import com.facebook.react.uimanager.ViewManagerDelegate
import com.facebook.react.uimanager.events.Event
import com.facebook.react.viewmanagers.DownstreamMarkdownViewManagerDelegate
import com.facebook.react.viewmanagers.DownstreamMarkdownViewManagerInterface
import com.facebook.yoga.YogaMeasureMode
import com.facebook.yoga.YogaMeasureOutput
import java.lang.ref.WeakReference

@ReactModule(name = DownstreamMarkdownViewManager.NAME)
class DownstreamMarkdownViewManager :
  SimpleViewManager<DownstreamMarkdownView>(),
  DownstreamMarkdownViewManagerInterface<DownstreamMarkdownView> {
  private val delegate = DownstreamMarkdownViewManagerDelegate(this)

  override fun getDelegate(): ViewManagerDelegate<DownstreamMarkdownView> = delegate

  override fun getName() = NAME

  override fun createViewInstance(context: ThemedReactContext): DownstreamMarkdownView {
    DownstreamNative.preparationContext = WeakReference(context)
    val view = DownstreamMarkdownView(context)
    // The view reports through the view that owns it, so a recycled view has no
    // emitter and a control that outlives its view reports nothing.
    view.onLinkPress = { target, url, start, end -> dispatchLinkPress(target, url, start, end) }
    view.onControl = { target, block, action, format, language, filename ->
      dispatchControl(target, block, action, format, language, filename)
    }
    view.onControlFailure = { target, block, action, message ->
      dispatchControlFailure(target, block, action, message)
    }
    view.onWrapToggle = { target, wrapped -> dispatchWrapToggle(target, wrapped) }
    return view
  }

  // The shadow node parses these props; the view reads the result from state.
  override fun setMarkdown(view: DownstreamMarkdownView, value: String?) = Unit

  override fun setGeneration(view: DownstreamMarkdownView, value: Int) {
    view.setGeneration(value)
  }

  override fun setSessionId(view: DownstreamMarkdownView, value: String?) {
    view.setSessionId(value)
  }

  override fun setSessionSequence(view: DownstreamMarkdownView, value: Int) = Unit

  override fun setSessionRevision(view: DownstreamMarkdownView, value: Int) = Unit

  // The theme props are interned by the shadow node, and the interned theme
  // reaches the view as an id in its state, the way the document's id does. The
  // arrays are kept here only so a view that has been given a theme and has not
  // yet been given the state for it has something to intern itself, which is the
  // order props and state actually arrive in.
  override fun setThemeSlots(view: DownstreamMarkdownView, value: ReadableArray?) {
    view.themeSlots = value?.let { array ->
      IntArray(array.size()) { array.getInt(it) }
    }
  }

  override fun setThemeValues(view: DownstreamMarkdownView, value: ReadableArray?) {
    view.themeValues = value?.let { array ->
      DoubleArray(array.size()) { array.getDouble(it) }
    }
  }

  override fun setThemeFonts(view: DownstreamMarkdownView, value: ReadableArray?) {
    view.themeFonts = value?.let { array ->
      Array(array.size()) { array.getString(it).orEmpty() }
    }
  }

  override fun setSelectable(view: DownstreamMarkdownView, value: Boolean) {
    view.setSelectable(value)
  }

  override fun setSyntaxHighlighting(view: DownstreamMarkdownView, value: Boolean) {
    view.setSyntaxHighlighting(value)
  }

  override fun setCodeBlockWrap(view: DownstreamMarkdownView, value: Boolean) {
    view.setCodeBlockWrap(value)
  }

  override fun setControls(view: DownstreamMarkdownView, value: Int) {
    view.setControls(value)
  }

  override fun setCodeFilename(view: DownstreamMarkdownView, value: String?) {
    view.setCodeFilename(value ?: "file")
  }

  override fun setTableFilename(view: DownstreamMarkdownView, value: String?) {
    view.setTableFilename(value ?: "table")
  }

  override fun setCsvSeparator(view: DownstreamMarkdownView, value: String?) {
    view.setCsvSeparator(value ?: ",")
  }

  override fun setControlLabels(view: DownstreamMarkdownView, value: ReadableArray?) {
    view.setControlLabels(value?.let { array ->
      List(array.size()) { index -> array.getString(index).orEmpty() }
    } ?: emptyList())
  }

  /**
   * The document, the chrome it lays out with, and the reader's wrap choices
   * all arrive here, because a Fabric state update is the one path a native
   * view has for changing its own layout without a React render.
   */
  override fun updateState(
    view: DownstreamMarkdownView,
    props: ReactStylesDiffMap,
    stateWrapper: StateWrapper,
  ): Any? {
    val state = stateWrapper.stateData ?: return null
    view.setStateWrapper(stateWrapper)
    val toggles = state.getArray("wrapToggles")
    val ordinals = mutableSetOf<Int>()
    if (toggles != null) {
      for (index in 0 until toggles.size()) {
        ordinals.add(toggles.getDouble(index).toInt())
      }
    }
    // The prop's own wrap setting travels beside the set, because together they
    // decide each block's layout and neither half can be inferred from `chrome`.
    view.setCodeBlockWrap((state.getDouble("codeBlockWrap") ?: 0.0) != 0.0)
    view.setCodeWrapToggles(ordinals)
    view.setChrome(state.getDouble("chrome").toInt())
    // The theme travels as its id, the way the document does, so a state update
    // carries one number rather than every slot the theme sets.
    view.setDocument(
      state.getDouble("documentId").toLong(),
      state.getDouble("fontScale").toFloat(),
      state.getDouble("themeId")?.toLong() ?: 0L,
    )
    return null
  }

  override fun prepareToRecycleView(reactContext: ThemedReactContext, view: DownstreamMarkdownView): DownstreamMarkdownView {
    super.prepareToRecycleView(reactContext, view)
    view.reset()
    return view
  }

  override fun measure(
    context: Context,
    localData: ReadableMap?,
    props: ReadableMap?,
    state: ReadableMap?,
    width: Float,
    widthMode: YogaMeasureMode?,
    height: Float,
    heightMode: YogaMeasureMode?,
    attachmentsPositions: FloatArray?,
  ): Long {
    val documentId = localData?.getDouble("documentId")?.toLong() ?: 0L
    val fontScale = localData?.getDouble("fontScale")?.toFloat() ?: 1f
    val chrome = localData?.getDouble("chrome")?.toInt() ?: 0
    // The theme id the view mounts with, so this measures the same rendering the
    // view draws rather than an unthemed one it would measure again.
    val themeId = localData?.getDouble("themeId")?.toLong() ?: 0L
    // The reader's wrap choices travel in the state beside the document, because
    // a code block's height depends on whether it wraps and a document can hold
    // both kinds at once. Measuring without them would measure a different
    // layout from the one the view draws.
    val codeBlockWrap = (localData?.getDouble("codeBlockWrap") ?: 0.0) != 0.0
    val toggles = localData?.getArray("wrapToggles")
    val wrapToggles = if (toggles == null) {
      IntArray(0)
    } else {
      IntArray(toggles.size()) { toggles.getDouble(it).toInt() }
    }
    if (DownstreamNative.preparationContext?.get() !== context) {
      DownstreamNative.preparationContext = WeakReference(context)
    }
    val rendered = RenderedDocument.obtain(context, documentId, fontScale, themeId = themeId)
      ?: return YogaMeasureOutput.make(0f, 0f)
    val layoutWidth = if (widthMode == YogaMeasureMode.UNDEFINED || !width.isFinite()) {
      rendered.desiredWidth(chrome, codeBlockWrap, wrapToggles)
    } else {
      width.toInt()
    }
    return YogaMeasureOutput.make(
      PixelUtil.toDIPFromPixel(layoutWidth.toFloat()),
      PixelUtil.toDIPFromPixel(
        rendered.height(layoutWidth, chrome, codeBlockWrap, wrapToggles).toFloat(),
      ),
    )
  }

  override fun getExportedCustomDirectEventTypeConstants(): MutableMap<String, Any> = mutableMapOf(
    LinkPressEvent.NAME to mapOf("registrationName" to "onLinkPress"),
    "topMarkdownError" to mapOf("registrationName" to "onMarkdownError"),
    "topControlAction" to mapOf("registrationName" to "onControlAction"),
  )

  private fun dispatch(view: DownstreamMarkdownView, event: Event<out Event<*>>) {
    val context = view.context as? ReactContext ?: return
    UIManagerHelper.getEventDispatcherForReactTag(context, view.id)?.dispatchEvent(event)
  }

  private fun dispatchLinkPress(view: DownstreamMarkdownView, url: String, start: Int, end: Int) {
    val context = view.context as? ReactContext ?: return
    val surfaceId = UIManagerHelper.getSurfaceId(context)
    dispatch(view, LinkPressEvent(surfaceId, view.id, url, start, end))
  }

  private fun dispatchControl(
    view: DownstreamMarkdownView,
    block: String,
    action: String,
    format: String,
    language: String,
    filename: String,
  ) {
    val context = view.context as? ReactContext ?: return
    dispatch(
      view,
      ControlActionEvent(
        UIManagerHelper.getSurfaceId(context),
        view.id,
        block,
        action,
        format,
        language,
        filename,
        false,
        view.currentGeneration(),
      ),
    )
  }

  /**
   * A control failure arrives on the existing error path, tagged with the block
   * and action it came from, so a per-control onError and the view's own onError
   * can both find it without a second event and without reading the message.
   */
  private fun dispatchControlFailure(view: DownstreamMarkdownView, block: String, action: String, message: String) {
    val context = view.context as? ReactContext ?: return
    val generation = view.currentGeneration()
    dispatch(
      view,
      MarkdownErrorEvent(
        UIManagerHelper.getSurfaceId(context),
        view.id,
        "control_failed",
        "$block $action failed: $message",
        generation,
        block,
        action,
      ),
    )
  }

  private fun dispatchWrapToggle(view: DownstreamMarkdownView, wrapped: Boolean) {
    // The height changed, and only Yoga and the chat list around the message can
    // know that, so the change goes back through a Fabric state update rather
    // than a React render: no parse, no JavaScript work, and no re-measure of
    // the text, which is laid out again only for the block that changed.
    val ordinal = view.nextUnwrappedOrdinal()
    if (ordinal < 0) return
    view.toggleWrap(ordinal)
    val context = view.context as? ReactContext ?: return
    dispatch(
      view,
      ControlActionEvent(
        UIManagerHelper.getSurfaceId(context),
        view.id,
        "code",
        "wrap",
        "",
        "",
        "",
        wrapped,
        view.currentGeneration(),
      ),
    )
  }

  companion object {
    const val NAME = "DownstreamMarkdownView"
  }
}

private class LinkPressEvent(
  surfaceId: Int,
  viewTag: Int,
  private val url: String,
  private val start: Int,
  private val end: Int,
) : Event<LinkPressEvent>(surfaceId, viewTag) {
  override fun getEventName() = NAME

  override fun getEventData(): WritableMap = Arguments.createMap().apply {
    putString("url", url)
    putInt("start", start)
    putInt("end", end)
  }

  companion object {
    const val NAME = "topLinkPress"
  }
}

private class ControlActionEvent(
  surfaceId: Int,
  viewTag: Int,
  private val block: String,
  private val action: String,
  private val format: String,
  private val language: String,
  private val filename: String,
  private val wrapped: Boolean = false,
  private val generation: Int,
) : Event<ControlActionEvent>(surfaceId, viewTag) {
  override fun getEventName() = NAME

  override fun getEventData(): WritableMap = Arguments.createMap().apply {
    putString("block", block)
    putString("action", action)
    putString("format", format)
    putString("language", language)
    putString("filename", filename)
    putBoolean("wrapped", wrapped)
    putInt("generation", generation)
  }

  companion object {
    const val NAME = "topControlAction"
  }
}

private class MarkdownErrorEvent(
  surfaceId: Int,
  viewTag: Int,
  private val code: String,
  private val message: String,
  private val generation: Int,
  private val block: String = "",
  private val action: String = "",
) : Event<MarkdownErrorEvent>(surfaceId, viewTag) {
  override fun getEventName() = NAME

  override fun getEventData(): WritableMap = Arguments.createMap().apply {
    putString("code", code)
    putString("message", message)
    putInt("generation", generation)
    putInt("sequence", 0)
    putBoolean("recoverable", true)
    putString("block", block)
    putString("action", action)
  }

  companion object {
    const val NAME = "topMarkdownError"
  }
}
