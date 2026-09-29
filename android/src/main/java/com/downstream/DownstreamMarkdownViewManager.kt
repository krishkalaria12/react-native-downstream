package com.downstream

import android.content.Context
import com.facebook.react.bridge.Arguments
import com.facebook.react.bridge.ReactContext
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
    return DownstreamMarkdownView(context).apply { onLinkPress = ::dispatchLinkPress }
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

  override fun setSelectable(view: DownstreamMarkdownView, value: Boolean) {
    view.setSelectable(value)
  }

  override fun updateState(
    view: DownstreamMarkdownView,
    props: ReactStylesDiffMap,
    stateWrapper: StateWrapper,
  ): Any? {
    val state = stateWrapper.stateData ?: return null
    view.setDocument(state.getDouble("documentId").toLong(), state.getDouble("fontScale").toFloat())
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
    if (DownstreamNative.preparationContext?.get() !== context) {
      DownstreamNative.preparationContext = WeakReference(context)
    }
    val rendered = RenderedDocument.obtain(context, documentId, fontScale)
      ?: return YogaMeasureOutput.make(0f, 0f)
    val layoutWidth = if (widthMode == YogaMeasureMode.UNDEFINED || !width.isFinite()) {
      rendered.desiredWidth()
    } else {
      width.toInt()
    }
    return YogaMeasureOutput.make(
      PixelUtil.toDIPFromPixel(layoutWidth.toFloat()),
      PixelUtil.toDIPFromPixel(rendered.height(layoutWidth).toFloat()),
    )
  }

  override fun getExportedCustomDirectEventTypeConstants(): MutableMap<String, Any> = mutableMapOf(
    LinkPressEvent.NAME to mapOf("registrationName" to "onLinkPress"),
    "topMarkdownError" to mapOf("registrationName" to "onMarkdownError"),
  )

  private fun dispatchLinkPress(view: DownstreamMarkdownView, url: String, start: Int, end: Int) {
    val context = view.context as? ReactContext ?: return
    val surfaceId = UIManagerHelper.getSurfaceId(context)
    UIManagerHelper.getEventDispatcherForReactTag(context, view.id)
      ?.dispatchEvent(LinkPressEvent(surfaceId, view.id, url, start, end))
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
