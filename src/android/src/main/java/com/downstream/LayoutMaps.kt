package com.downstream

import com.facebook.react.bridge.Arguments
import com.facebook.react.bridge.ReadableMap
import com.facebook.react.uimanager.PixelUtil

/**
 * Reading the two layouts out of the map `codeLayoutData` builds in
 * `DownstreamTextLayoutManager.cpp`.
 *
 * One file for both, and one shape for both, because `measure`, `updateState`, and
 * the worker `prepare` all carry the same map: a measurement, a mount, and a
 * background preparation that disagree about a height are three layouts, and the
 * whole point of one map is that they cannot.
 *
 * Separate from `ImageLayout.kt` and `CodeLayout.kt`, which are the mirrors and
 * compile without React Native on the classpath. That is what lets
 * `pnpm test:kotlin` run their tables against the C++ ones with nothing but a
 * Kotlin compiler. The bridge types belong here, where React Native is a given.
 */

/**
 * A number from the map, or [fallback] when the key is absent or null.
 *
 * `ReadableMap.getDouble` throws `NoSuchKeyException` for a missing key rather
 * than returning null, so every read of an optional key goes through here. The
 * state map and the measure map are written by two C++ functions, and a key one
 * of them does not write must be a default, not a crash on mount.
 */
internal fun ReadableMap.numberAt(key: String, fallback: Double): Double =
  if (hasKey(key) && !isNull(key)) getDouble(key) else fallback

/** The code layout in the map, as the shadow node resolved it. */
internal fun ReadableMap.toCodeLayout(): CodeLayout = CodeLayout(
  CodeLayoutProps(
    numberAt("codeBlockWrap", 0.0) != 0.0,
    numberAt("lineNumbers", 1.0) != 0.0,
    PixelUtil.toPixelFromDIP(numberAt("codeBlockMaxHeight", 400.0)).toFloat(),
  ),
  CodeChoices(ordinalsAt("wrapToggles"), ordinalsAt("expanded")),
)

/** The table layout in the map, as the shadow node resolved it. */
internal fun ReadableMap.toTableLayout(): TableLayout = TableLayout(
  TableLayoutProps(PixelUtil.toPixelFromDIP(numberAt("tableMaxHeight", 300.0)).toFloat()),
  TableChoices(ordinalsAt("tableExpanded")),
)

/**
 * The image layout in the map, as the shadow node resolved it.
 *
 * The round trip is entries and prefixes together, because only a measurement has
 * to decide whether a URL may be fetched — the view is handed a state that already
 * says blocked. So a change to the prefix list changes an entry, which is what
 * makes the shadow node notice and relayout, rather than a field the view draws
 * from but the shadow node never reads.
 */
internal fun ReadableMap.toImageLayout(): ImageLayout = ImageLayout(
  state = ImageStateLayout(
    enabled = numberAt("imagesEnabled", 1.0) != 0.0,
    entries = imageEntriesAt("imageEntries"),
  ),
  allowedPrefixes = stringsAt("allowedImagePrefixes"),
)

/**
 * The hosted React children from the map, as [HostLayout] expects them.
 *
 * Heights cross as points, as the shadow node measures them, and are converted
 * to pixels here once, the same place the image sizes are converted, so nothing
 * downstream in this view has to know which unit it is holding.
 */
internal fun ReadableMap.toHostLayout(): HostLayout = HostLayout(
  entries = hostEntriesAt("hostEntries"),
)

internal fun ReadableMap.hostEntriesAt(key: String): List<HostEntry> {
  val array = (if (hasKey(key)) getArray(key) else null) ?: return emptyList()
  return List(array.size()) { index ->
    val entry = array.getMap(index) ?: Arguments.createMap()
    HostEntry(
      ordinal = entry.numberAt("ordinal", 0.0).toInt(),
      height = PixelUtil.toPixelFromDIP(entry.numberAt("height", 0.0)).toFloat(),
    )
  }
}

/**
 * One sorted choice set from the map.
 *
 * Sorted here rather than trusted, because the shadow node builds these from the
 * reader's toggles and a set that is sorted on one side and not the other is a
 * `binarySearch` that silently misses.
 */
internal fun ReadableMap.ordinalsAt(key: String): IntArray {
  val array = (if (hasKey(key)) getArray(key) else null) ?: return IntArray(0)
  return IntArray(array.size()) { array.getDouble(it).toInt() }.also { it.sort() }
}

/** A string array from the map, or nothing when the key is absent. */
internal fun ReadableMap.stringsAt(key: String): List<String> {
  val array = (if (hasKey(key)) getArray(key) else null) ?: return emptyList()
  return List(array.size()) { index -> array.getString(index).orEmpty() }
}

/**
 * The image entries from the map, as [ImageStateLayout] expects them.
 *
 * The engine writes sorted by ordinal, which is what `ImageStateLayout.find`
 * binary-searches over, so the order is kept rather than re-sorted here. A missing
 * element becomes an empty map and therefore an entry of defaults, because every
 * field is defaulted and a null would push that decision onto every reader.
 *
 * The engine records an image's size in points, as the shadow node measures, and
 * every metric on this side is in pixels, so the size is converted here once.
 */
internal fun ReadableMap.imageEntriesAt(key: String): List<ImageEntry> {
  val array = (if (hasKey(key)) getArray(key) else null) ?: return emptyList()
  return List(array.size()) { index ->
    val entry = array.getMap(index) ?: Arguments.createMap()
    ImageEntry(
      ordinal = entry.numberAt("ordinal", 0.0).toInt(),
      state = ImageState.of(entry.numberAt("state", 0.0).toInt()) ?: ImageState.PENDING,
      pixelWidth = PixelUtil.toPixelFromDIP(entry.numberAt("pixelWidth", 0.0)).toFloat(),
      pixelHeight = PixelUtil.toPixelFromDIP(entry.numberAt("pixelHeight", 0.0)).toFloat(),
    )
  }
}