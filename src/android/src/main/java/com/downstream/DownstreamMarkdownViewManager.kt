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
import com.facebook.react.uimanager.ViewGroupManager
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
/**
 * The view manager for <DownstreamMarkdown>.
 *
 * A `ViewGroupManager` rather than a `SimpleViewManager` because this view can
 * now have children: Fabric mounts one `DownstreamFenceSlot` per hosted fence on
 * it, and a manager that accepts no children would drop them. The four child
 * methods below address the view's host layer, which is the layer Fabric's
 * children belong in, so the document's own segment views — reconciled by
 * document index inside the view — and React's children — reconciled in React's
 * order here — never see each other.
 *
 * `needsCustomLayoutForChildren()` is false, so Fabric lays the host children out
 * at the frames the shadow node's measurement gave them and nothing here moves
 * them.
 */
class DownstreamMarkdownViewManager :
  ViewGroupManager<DownstreamMarkdownView>(),
  DownstreamMarkdownViewManagerInterface<DownstreamMarkdownView> {
  private val delegate = DownstreamMarkdownViewManagerDelegate(this)

  override fun getDelegate(): ViewManagerDelegate<DownstreamMarkdownView> = delegate

  override fun getName() = NAME

  override fun addView(parent: DownstreamMarkdownView, child: android.view.View, index: Int) {
    parent.hostLayer.addView(child, index)
  }

  override fun getChildCount(parent: DownstreamMarkdownView): Int = parent.hostLayer.childCount

  override fun getChildAt(parent: DownstreamMarkdownView, index: Int): android.view.View? =
    parent.hostLayer.getChildAt(index)

  override fun removeViewAt(parent: DownstreamMarkdownView, index: Int) {
    val child = parent.hostLayer.getChildAt(index) ?: return
    parent.hostLayer.removeViewAt(index)
    child.parent?.let { parent.hostLayer.removeView(child) }
  }

  override fun removeAllViews(parent: DownstreamMarkdownView) {
    parent.hostLayer.removeAllViews()
  }

  override fun needsCustomLayoutForChildren(): Boolean = false

  override fun onAfterUpdateTransaction(parent: DownstreamMarkdownView) {
    super.onAfterUpdateTransaction(parent)
    // A child mounted in this transaction has its frame from the shadow node
    // already; this only asks the layer for a layout pass so it is not a frame
    // behind the state update that placed it.
    parent.layoutHostChildren()
  }

  override fun createViewInstance(context: ThemedReactContext): DownstreamMarkdownView {
    DownstreamNative.preparationContext = WeakReference(context)
    val view = DownstreamMarkdownView(context)
    // The view reports through the view that owns it, so a recycled view has no
    // emitter and a control that outlives its view reports nothing.
    view.onLinkPress = { target, url, start, end -> dispatchLinkPress(target, url, start, end) }
    view.onImagePress = { target, url, alt, title, range -> dispatchImagePress(target, url, alt, title, range) }
    view.onControl = { target, block, action, format, language, filename, url ->
      dispatchControl(target, block, action, format, language, filename, url)
    }
    view.onControlFailure = { target, block, action, message ->
      dispatchControlFailure(target, block, action, message)
    }
    view.onWrapToggle = { target, ordinal, wrapped -> dispatchWrapToggle(target, ordinal, wrapped) }
    view.onExpandToggle = { target, ordinal, expanded -> dispatchExpandToggle(target, ordinal, expanded) }
    // Phase 2. A table's two buttons report as `table`, and the full-screen one
    // carries no ordinal: JavaScript already holds the table's Markdown, and an
    // event that repeated it would put a whole table into an event payload.
    view.onTableExpandToggle = { target, ordinal, expanded ->
      dispatchTableExpandToggle(target, ordinal, expanded)
    }
    view.onTableFullscreen = { target, ordinal -> dispatchTableFullscreen(target, ordinal) }
    view.onDetailsToggle = { target, containerId, open ->
      dispatchDetailsToggle(target, containerId, open)
    }
    view.onTaskToggle = { target, containerId, checked ->
      dispatchTaskToggle(target, containerId, checked)
    }
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

  /**
   * The `dir` prop, which overrides the engine's per-block decision.
   *
   * Read on the view rather than passed down: the shadow node folds it into the
   * layout hint, so a measurement already carries it, and what the view needs is
   * only the direction to draw text in. `auto` is the engine's own answer, which
   * is what a document with no `dir` prop gets.
   */
  override fun setDir(view: DownstreamMarkdownView, value: String?) {
    view.setDirection(value)
  }

  override fun setSyntaxHighlighting(view: DownstreamMarkdownView, value: Boolean) {
    view.setSyntaxHighlighting(value)
  }

  // The shadow node parses these into the state; the view reads the result from
  // there, because a height belongs to the measurement rather than to a prop.
  override fun setCodeBlockWrap(view: DownstreamMarkdownView, value: Boolean) = Unit

  /**
   * The effect, the split and the timing are resolved once in `src/animation.ts`
   * and arrive as three props. The view joins them into the one array the engine
   * reads, with the animator scale appended.
   */
  override fun setAnimation(view: DownstreamMarkdownView, value: String?) = view.setAnimationEffect(value)

  override fun setAnimationSplit(view: DownstreamMarkdownView, value: String?) = view.setAnimationSplit(value)

  override fun setAnimationTiming(view: DownstreamMarkdownView, value: ReadableArray?) {
    view.setAnimationValues(value?.let { array ->
      DoubleArray(array.size()) { array.getDouble(it) }
    })
  }

  /**
   * `auto`, `on` and `off` reach the engine as part of the caret, which the state
   * carries already resolved against the document's own streaming flag. A
   * controlled view has no stream, so the flag is the whole answer and there is
   * nothing for the view to do with the string.
   */
  override fun setAnimating(view: DownstreamMarkdownView, value: String?) = Unit

  override fun setCaret(view: DownstreamMarkdownView, value: String?) = Unit

  override fun setLineNumbers(view: DownstreamMarkdownView, value: Boolean) = Unit

  override fun setCodeBlockMaxHeight(view: DownstreamMarkdownView, value: Double) = Unit

  override fun setControls(view: DownstreamMarkdownView, value: Int) {
    view.setControls(value)
  }

  override fun setCodeFilename(view: DownstreamMarkdownView, value: String?) {
    view.setCodeFilename(value ?: "file")
  }

  override fun setTableFilename(view: DownstreamMarkdownView, value: String?) {
    view.setTableFilename(value ?: "table")
  }

  override fun setImageFilename(view: DownstreamMarkdownView, value: String?) {
    view.setImageFilename(value ?: "image")
  }

  /**
   * The parse flags, and the `images` switch, are props the shadow node parses: it
   * decides whether a formula resolves, whether a tag is a break or a script, and
   * whether a URL may be fetched at all, and it writes those answers into the state
   * this view draws from. The view keeps the flags for the one decision the state
   * does not carry — what a picture's save is named — and nothing else.
   */
  override fun setParseFlags(view: DownstreamMarkdownView, value: Int) {
    view.setParseFlags(value)
  }

  /**
   * The HTML tags the source may use, in the engine's wire form.
   *
   * Only the shadow node reads them, and it reads them from the props itself: it is
   * the only thing that parses, so there is nothing for the view to keep. The
   * override exists because codegen declares the prop and a manager that did not
   * implement it would fail to compile.
   */
  override fun setAllowedTags(view: DownstreamMarkdownView, value: ReadableArray?) {
    // Intentionally empty. See above.
  }

  /**
   * The tallest a table grows before it scrolls, in points; 0 is no cap.
   *
   * A height, so it reaches the view the way `codeBlockMaxHeight` does: through
   * the shadow node's state, which is the one path a native view has for changing
   * its own layout without a React render. The override is here so the manager
   * implements every prop codegen declared.
   */
  override fun setTableMaxHeight(view: DownstreamMarkdownView, value: Double) {
    // Intentionally empty. See above.
  }

  /**
   * The origin a relative link resolves against.
   *
   * The engine keeps the source's own `href` on a press and JavaScript resolves
   * it, so nothing here reads the value: it is a parse bit that arrives in the
   * same `parseFlags` prop. The override exists so the manager implements every
   * prop codegen declared.
   */
  override fun setLinkOrigin(view: DownstreamMarkdownView, value: String?) {
    // Intentionally empty. See above.
  }

  override fun setImages(view: DownstreamMarkdownView, value: Boolean) {
    view.setImages(value)
  }

  /**
   * The prefix list, which only a measurement reads: the state already says which
   * destinations were blocked. The view does not re-check it, because a view that
   * fetched a URL the measurement refused would draw a picture inside a box that was
   * reserved for a line of alt text.
   */
  override fun setAllowedImagePrefixes(view: DownstreamMarkdownView, value: ReadableArray?) {
    view.setAllowedImagePrefixes(value?.let { array ->
      List(array.size()) { index -> array.getString(index).orEmpty() }
    } ?: emptyList())
  }

  override fun setCsvSeparator(view: DownstreamMarkdownView, value: String?) {
    view.setCsvSeparator(value ?: ",")
  }

  /**
   * The `icons` prop's resolved names, which the button builders read for their
   * glyph.
   *
   * A name the app set to an empty string is a removed glyph, and a text-only
   * button is what that is for. Anything this library does not recognise falls
   * back to the platform's own, so a name an app invented cannot leave a button
   * with no icon at all.
   */
  override fun setControlIcons(view: DownstreamMarkdownView, value: ReadableArray?) {
    setControlIcons(value?.let { array ->
      List(array.size()) { index -> array.getString(index).orEmpty() }
    } ?: emptyList())
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
    // The whole code layout arrives in one map: the props and the reader's own
    // per-block choices together, because between them they decide each card's
    // height and neither half can be inferred from `chrome`.
    view.setCodeProps(
      state.numberAt("codeBlockWrap", 0.0) != 0.0,
      state.numberAt("lineNumbers", 1.0) != 0.0,
      state.numberAt("codeBlockMaxHeight", 400.0),
    )
    view.setCodeChoices(
      state.ordinalsAt("wrapToggles"),
      state.ordinalsAt("expanded"),
    )
    // Images resolve the same way code does: the state carries each picture's
    // answer and the prefix list that produced it, so a reload draws the box the
    // measurement reserved for it rather than a placeholder under a loaded image.
    view.setImageLayout(state.toImageLayout())
    // The hosted React children, in the same state: a fence with a child is drawn
    // as a host segment at that child's height, and one without keeps its card.
    view.setHostLayout(state.toHostLayout())
    // The table layout arrives the same way the code layout does, because it is
    // the same decision about a different block: a cap and the reader's choices.
    view.setTableProps(state.numberAt("tableMaxHeight", 300.0))
    view.setTableChoices(state.ordinalsAt("tableExpanded"))
    view.setDetailsChoices(state.ordinalsAt("detailsOpen"))
    view.setChrome(state.numberAt("chrome", 0.0).toInt())
    // The caret is resolved in the node, against the document and its streaming
    // flag, so the view never decides where a caret goes. It is a state field
    // rather than a prop for the same reason `chrome` is: its placeholder is
    // measured with the text, so a change reaches the view through a state update
    // on the next layout.
    view.setCaret(
      PlacedCaret(
        state.numberAt("caretKind", 0.0).toInt(),
        state.numberAt("caretPlace", 0.0).toInt(),
      ),
    )
    // The theme travels as its id, the way the document does, so a state update
    // carries one number rather than every slot the theme sets.
    view.setDocument(
      state.numberAt("documentId", 0.0).toLong(),
      state.numberAt("fontScale", 1.0).toFloat(),
      state.numberAt("themeId", 0.0).toLong(),
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
    val documentId = localData?.numberAt("documentId", 0.0)?.toLong() ?: 0L
    val fontScale = localData?.numberAt("fontScale", 1.0)?.toFloat() ?: 1f
    val chrome = localData?.numberAt("chrome", 0.0)?.toInt() ?: 0
    // The theme id the view mounts with, so this measures the same rendering the
    // view draws rather than an unthemed one it would measure again.
    val themeId = localData?.numberAt("themeId", 0.0)?.toLong() ?: 0L
    // The whole code layout travels in the state beside the document, because a
    // code block's height depends on whether it wraps, whether its cap applies, and
    // whether it has a gutter, and one document can hold every combination at once.
    // Measuring without them would measure a different layout from the one the view
    // draws.
    val code = localData?.toCodeLayout() ?: CodeLayout()
    // The image answers travel in the same map, for the same reason: an image's
    // height is its state and its size, and measuring without them would measure
    // a placeholder from a layout the view is not drawing.
    val images = localData?.toImageLayout() ?: ImageLayout()
    // The hosted children's heights travel in the same map, for the same reason:
    // a fence drawn by a React component is as tall as that component, and
    // measuring without it would measure the card the child replaces.
    val hosts = localData?.toHostLayout() ?: HostLayout()
    if (DownstreamNative.preparationContext?.get() !== context) {
      DownstreamNative.preparationContext = WeakReference(context)
    }
    // The caret is measured with everything else: its placeholder is a real
    // character in the text, so measuring without it would put the last line
    // somewhere the view then does not draw it.
    val caret = PlacedCaret(
      localData?.getDouble("caretKind")?.toInt() ?: 0,
      localData?.getDouble("caretPlace")?.toInt() ?: 0,
    )
    val rendered = RenderedDocument.obtain(context, documentId, fontScale, themeId = themeId, caret = caret)
      ?: return YogaMeasureOutput.make(0f, 0f)
    val layoutWidth = if (widthMode == YogaMeasureMode.UNDEFINED || !width.isFinite()) {
      rendered.desiredWidth(code, images)
    } else {
      width.toInt()
    }
    return YogaMeasureOutput.make(
      PixelUtil.toDIPFromPixel(layoutWidth.toFloat()),
      PixelUtil.toDIPFromPixel(rendered.height(layoutWidth, chrome, code, images, hosts).toFloat()),
    )
  }

  override fun getExportedCustomDirectEventTypeConstants(): MutableMap<String, Any> = mutableMapOf(
    LinkPressEvent.NAME to mapOf("registrationName" to "onLinkPress"),
    ImagePressEvent.NAME to mapOf("registrationName" to "onImagePress"),
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

  private fun dispatchImagePress(view: DownstreamMarkdownView, url: String, alt: String, title: String, range: IntRange) {
    val context = view.context as? ReactContext ?: return
    val surfaceId = UIManagerHelper.getSurfaceId(context)
    dispatch(view, ImagePressEvent(surfaceId, view.id, url, alt, title, range.first, range.last))
  }

  private fun dispatchControl(
    view: DownstreamMarkdownView,
    block: String,
    action: String,
    format: String,
    language: String,
    filename: String,
    url: String,
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
        // A copy, download or format action leaves both choices as they were, so
        // the reader's state is reported rather than implied.
        false,
        false,
        view.currentGeneration(),
        url,
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

  /**
   * A wrap or expand press changes a card's height, and only Yoga and the chat
   * list around the message can know that, so the change goes back through a Fabric
   * state update rather than a React render: no parse, no JavaScript work, and no
   * re-measure of the text, which is laid out again only for the block that
   * changed. The button has already acted by the time the event is reported.
   */
  private fun dispatchWrapToggle(view: DownstreamMarkdownView, ordinal: Int, wrapped: Boolean) {
    val toggled = view.nextToggledOrdinal()
    if (toggled < 0) return
    view.toggleWrap(ordinal)
    dispatchControlAction(view, "wrap", wrapped = wrapped, ordinal = ordinal.toDouble())
  }

  private fun dispatchExpandToggle(view: DownstreamMarkdownView, ordinal: Int, expanded: Boolean) {
    val toggled = view.nextToggledOrdinal()
    if (toggled < 0) return
    view.toggleExpand(ordinal)
    dispatchControlAction(
      view,
      "expand",
      expanded = expanded,
      ordinal = ordinal.toDouble(),
    )
  }

  /**
   * A table's expand press. The same state round trip as a code card's, for the
   * same reason: only Yoga and the chat list know the table's new height.
   */
  private fun dispatchTableExpandToggle(
    view: DownstreamMarkdownView,
    ordinal: Int,
    expanded: Boolean,
  ) {
    val toggled = view.nextToggledOrdinal()
    if (toggled < 0) return
    view.toggleTableExpand(ordinal)
    dispatchControlAction(
      view,
      "expand",
      block = "table",
      expanded = expanded,
      ordinal = ordinal.toDouble(),
    )
  }

  /**
   * A table's full-screen press. No state update, because nothing about the
   * message changes: a modal holding a second view is the whole of the effect,
   * and the height that view needs is the width of the screen rather than the
   * width of this column.
   */
  private fun dispatchTableFullscreen(view: DownstreamMarkdownView, ordinal: Int) {
    val toggled = view.nextToggledOrdinal()
    if (toggled < 0) return
    val context = view.context as? ReactContext ?: return
    dispatch(
      view,
      ControlActionEvent(
        UIManagerHelper.getSurfaceId(context),
        view.id,
        "table",
        "fullscreen",
        "",
        "",
        "",
        false,
        false,
        view.currentGeneration(),
        ordinal = ordinal.toDouble(),
      ),
    )
  }

  /**
   * A `<details>` summary press.
   *
   * The same shape as a wrap or an expand press: the view has already applied its
   * own answer, the state update is the thing that makes the chat list re-measure,
   * and the event reports which way it went. `block` is `details` because the
   * three kinds of collapsible choice are three different blocks.
   */
  private fun dispatchDetailsToggle(
    view: DownstreamMarkdownView,
    containerId: Int,
    open: Boolean,
  ) {
    val toggled = view.nextToggledOrdinal()
    if (toggled < 0) return
    view.toggleDetails(containerId)
    dispatchControlAction(
      view,
      "expand",
      block = "details",
      expanded = open,
      ordinal = containerId.toDouble(),
    )
  }

  /**
   * A task checkbox press.
   *
   * No state update, because nothing about the message changes: the checkbox is
   * drawn, not stored, and the app owns the source that flips it. The event is
   * the whole of the effect.
   */
  private fun dispatchTaskToggle(view: DownstreamMarkdownView, containerId: Int, checked: Boolean) {
    val toggled = view.nextToggledOrdinal()
    if (toggled < 0) return
    val context = view.context as? ReactContext ?: return
    dispatch(view, TaskPressEvent(
      UIManagerHelper.getSurfaceId(context),
      view.id,
      containerId,
      checked,
      view.currentGeneration(),
    ))
  }

  private fun dispatchControlAction(
    view: DownstreamMarkdownView,
    action: String,
    block: String = "code",
    wrapped: Boolean = false,
    expanded: Boolean = false,
    // The block's ordinal among its own kind, because an app that keys a press
    // needs to know which block was pressed and the three kinds count separately.
    // -1 where a press has no ordinal to report.
    ordinal: Double = -1.0,
  ) {
    val context = view.context as? ReactContext ?: return
    dispatch(
      view,
      ControlActionEvent(
        UIManagerHelper.getSurfaceId(context),
        view.id,
        block,
        action,
        "",
        "",
        "",
        wrapped,
        expanded,
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

/** A press on a loaded picture that is not inside a link, as iOS reports it. */
private class ImagePressEvent(
  surfaceId: Int,
  viewTag: Int,
  private val url: String,
  private val alt: String,
  private val title: String,
  private val start: Int,
  private val end: Int,
) : Event<ImagePressEvent>(surfaceId, viewTag) {
  override fun getEventName() = NAME

  override fun getEventData(): WritableMap = Arguments.createMap().apply {
    putString("url", url)
    putString("alt", alt)
    putString("title", title)
    putInt("start", start)
    putInt("end", end)
  }

  companion object {
    const val NAME = "topImagePress"
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
  private val expanded: Boolean = false,
  private val generation: Int,
  private val url: String = "",
  /**
   * The table this press belongs to, as its ordinal among the document's tables.
   *
   * A number rather than the Markdown, because JavaScript can identify the table
   * and a whole table in an event payload is a whole table through the bridge for
   * nothing: the modal is rendered from the `markdown` field of this same event,
   * which the native side already had to build.
   */
  private val ordinal: Double = -1.0,
) : Event<ControlActionEvent>(surfaceId, viewTag) {
  override fun getEventName() = NAME

  override fun getEventData(): WritableMap = Arguments.createMap().apply {
    putString("block", block)
    putString("action", action)
    putString("format", format)
    putString("language", language)
    putString("filename", filename)
    putString("url", url)
    putBoolean("wrapped", wrapped)
    putBoolean("expanded", expanded)
    putInt("generation", generation)
    putDouble("ordinal", ordinal)
  }

  companion object {
    const val NAME = "topControlAction"
  }
}

private class TaskPressEvent(
  surfaceId: Int,
  viewTag: Int,
  private val containerId: Int,
  private val checked: Boolean,
  private val generation: Int,
) : Event<TaskPressEvent>(surfaceId, viewTag) {
  override fun getEventName() = NAME

  override fun getEventData(): WritableMap = Arguments.createMap().apply {
    putInt("containerId", containerId)
    putBoolean("checked", checked)
    putInt("generation", generation)
  }

  companion object {
    const val NAME = "topTaskPress"
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
