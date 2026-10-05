package com.downstream

import kotlin.math.abs
import kotlin.system.exitProcess

/**
 * The table in `src/cpp/tests/test_images.cpp`, run against the Kotlin mirror.
 *
 * `ImageLayout.kt` is the platform's copy of `downstream/ImageLayout.h`, and the
 * shadow node measures with the C++ one while the view draws with this one, so
 * both sides have to answer every question the same way. Running one table
 * against both is what proves they do, rather than a comment saying they should.
 *
 * Every case and every expected number here is the case and the number in the C++
 * table, in the same order, so a change to one that is not a change to the other
 * shows up as a mismatch rather than as two files that drifted.
 *
 * Compiled and run by `pnpm test:kotlin`, which needs a Kotlin compiler and
 * therefore is not part of `pnpm test:native`.
 */
private var failures = 0

private fun check(condition: Boolean, label: String) {
  if (!condition) {
    println("FAIL $label")
    failures++
  }
}

private fun checkEq(actual: Int, expected: Int, label: String) =
  check(actual == expected, "$label: expected $expected, actual $actual")

/** The same tolerance `test_images.cpp` compares with. */
private fun near(actual: Float, expected: Float): Boolean {
  val scale = abs(expected)
  val magnitude = if (scale > 1f) scale else 1f
  return abs(actual - expected) <= 1e-3f * magnitude
}

private fun checkBox(actual: ImageBox, width: Float, height: Float, label: String) {
  if (!near(actual.width, width) || !near(actual.height, height)) {
    println("FAIL $label: expected ${width}x$height, actual ${actual.width}x${actual.height}")
    failures++
  }
}

private fun checkState(actual: ImageState, expected: ImageState, label: String) =
  check(actual == expected, "$label: expected $expected, actual $actual")

private fun checkAllowed(actual: Boolean, expected: Boolean, label: String) =
  check(actual == expected, "$label: expected ${readable(expected)}, actual ${readable(actual)}")

private fun readable(value: Boolean) = if (value) "allowed" else "blocked"

fun main() {
  // ------------------------------------------------------- the placeholder box
  val metrics = ImageMetrics()
  val pending = ImageEntry(0, ImageState.PENDING)
  val loading = ImageEntry(0, ImageState.LOADING)

  checkBox(imageBox(pending, 400f, metrics), 192f, 96f, "a pending image is the placeholder")
  checkBox(imageBox(loading, 400f, metrics), 192f, 96f, "a loading image is the placeholder too")
  checkBox(imageBox(null, 400f, metrics), 192f, 96f, "no answer is the placeholder")

  checkBox(imageBox(pending, 100f, metrics), 100f, 96f, "a narrow column clamps the width")
  checkBox(imageBox(pending, 60f, metrics), 60f, 60f, "a narrower column clamps both axes")
  checkBox(imageBox(pending, 0f, metrics), 0f, 0f, "an unmeasured column takes no room")
  checkBox(imageBox(pending, -10f, metrics), 0f, 0f, "a negative column takes no room")

  val wide = ImageMetrics(placeholderWidth = 300f, placeholderHeight = 150f)
  checkBox(imageBox(pending, 400f, wide), 300f, 150f, "the theme's placeholder size is used")

  // ------------------------------------------------- a size before the bytes
  val loadingSized = ImageEntry(0, ImageState.LOADING, 800f, 600f)
  checkBox(imageBox(loadingSized, 400f, metrics), 400f, 300f, "a known size is used while the bytes arrive")
  checkBox(
    imageBox(ImageEntry(0, ImageState.LOADING, 800f, 0f), 400f, metrics),
    192f, 96f, "half a size is not a size",
  )
  checkBox(
    imageBox(ImageEntry(0, ImageState.PENDING, 0f, 600f), 400f, metrics),
    192f, 96f, "a zero width is not a size",
  )

  // --------------------------------------------------------- a loaded image
  val loaded = ImageEntry(0, ImageState.LOADED, 800f, 600f)
  checkBox(imageBox(loaded, 400f, metrics), 400f, 300f, "a wide image scales to the column")
  checkBox(imageBox(loaded, 1600f, metrics), 800f, 600f, "an image smaller than the column is not blown up")
  checkBox(imageBox(loaded, 800f, metrics), 800f, 600f, "an image the width of the column is its own size")
  checkBox(imageBox(loaded, 799f, metrics), 799f, 599.25f, "one point narrower scales by a fraction")

  val capped = ImageMetrics(maxHeight = 300f)
  checkBox(imageBox(loaded, 1600f, capped), 400f, 300f, "the cap scales a tall image down")
  checkBox(imageBox(loaded, 400f, capped), 400f, 300f, "the cap and the column agree")
  val narrowCap = ImageMetrics(maxHeight = 150f)
  checkBox(imageBox(loaded, 1600f, narrowCap), 200f, 150f, "the tighter of the cap and the column wins")

  val tallCap = ImageMetrics(maxHeight = 5000f)
  checkBox(imageBox(loaded, 1600f, tallCap), 800f, 600f, "a cap taller than the image changes nothing")
  checkBox(imageBox(loaded, 1600f, metrics), 800f, 600f, "no cap leaves the image alone")

  checkBox(
    imageBox(ImageEntry(0, ImageState.LOADED, 0f, 0f), 400f, metrics),
    192f, 96f, "a loaded image with no pixels is the placeholder",
  )

  val tall = ImageEntry(0, ImageState.LOADED, 600f, 3000f)
  checkBox(imageBox(tall, 400f, metrics), 400f, 2000f, "a tall image with no cap scales by its width")
  checkBox(imageBox(tall, 400f, ImageMetrics(maxHeight = 400f)), 80f, 400f, "a cap is what a tall image scales by")
  checkBox(imageBox(tall, 400f, ImageMetrics(maxHeight = 150f)), 30f, 150f, "a tighter cap scales a tall image further")
  checkBox(imageBox(tall, 600f, metrics), 600f, 3000f, "a tall image the width of the column is its own size")

  // ------------------------------------------------ the states that are text
  val failed = ImageEntry(0, ImageState.FAILED)
  checkBox(imageBox(failed, 400f, metrics), 400f, 18f, "a failed image is one line")
  checkBox(imageBox(ImageEntry(0, ImageState.BLOCKED), 400f, metrics), 400f, 18f, "a blocked image is one line")
  checkBox(imageBox(ImageEntry(0, ImageState.DISABLED), 400f, metrics), 400f, 18f, "a disabled image is one line")
  checkBox(imageBox(failed, 100f, metrics), 100f, 18f, "a failed image takes the column's width")
  checkBox(imageBox(failed, 0f, metrics), 0f, 18f, "an unmeasured failed image still takes its line")
  checkBox(
    imageBox(ImageEntry(0, ImageState.FAILED, 800f, 600f), 400f, metrics),
    400f, 18f, "a failed image ignores a known size",
  )
  checkBox(
    imageBox(failed, 400f, ImageMetrics(fallbackTextHeight = 42f)),
    400f, 42f, "the theme sets the fallback line's height",
  )

  // ---------------------------------------------------------- the lookup
  val layout = ImageStateLayout(
    entries = listOf(
      ImageEntry(2, ImageState.LOADED, 10f, 20f),
      ImageEntry(0, ImageState.PENDING),
      ImageEntry(1, ImageState.FAILED),
    ).sortedBy { it.ordinal },
  )
  check(layout.find(0) != null, "an entry is found by its ordinal")
  check(layout.find(2) != null, "the last entry is found too")
  check(layout.find(3) == null, "an ordinal with no entry is not invented")
  check(layout.find(0)!!.state == ImageState.PENDING, "the right entry comes back")
  check(ImageStateLayout().find(0) == null, "an empty layout has no entries")
  check(layout.find(1)!!.state == ImageState.FAILED, "sorting by ordinal leaves every entry findable")
  val entries = List(8192) { ImageEntry(it * 2, ImageState.LOADED, 10f, 20f) }
  var reads = 0
  val counted = object : AbstractList<ImageEntry>() {
    override val size: Int get() = entries.size
    override fun get(index: Int): ImageEntry {
      reads++
      return entries[index]
    }
  }
  val sparse = ImageStateLayout(entries = counted)
  check(sparse.find(16382) == entries.last(), "a large sparse image layout finds its final entry")
  check(reads <= 14, "image lookup stays logarithmic")
  reads = 0
  check(sparse.find(16381) == null, "a missing image ordinal stays absent")
  check(reads <= 14, "missing image lookup stays logarithmic")
  check(sparse.find(-1) == null, "an ordinal before the first image stays absent")
  check(sparse.find(16384) == null, "an ordinal after the last image stays absent")

  checkEq(ImageState.LOADED.code, 2, "the state codes match the wire order")
  checkEq(ImageState.DISABLED.code, 5, "the disabled code matches the wire order")
  check(ImageState.of(3) == ImageState.FAILED, "a code round trips to its state")
  check(ImageState.of(9) == null, "an unknown code is no state")

  // -------------------------------------------------------- the state rule
  checkState(imageStateFor(false, true, true, true), ImageState.DISABLED, "off beats a known image")
  checkState(imageStateFor(false, false, false, false), ImageState.DISABLED, "off beats an unfinished one")
  checkState(imageStateFor(true, false, false, false), ImageState.PENDING, "an unfinished destination is pending")
  checkState(imageStateFor(true, false, true, false), ImageState.DISABLED, "a settled empty destination is disabled")
  checkState(imageStateFor(true, true, true, true), ImageState.LOADED, "a known image is loaded")
  checkState(imageStateFor(true, true, true, false), ImageState.LOADING, "an unknown image is loading")
  checkState(imageStateFor(true, true, false, true), ImageState.LOADED, "a mid-stream image is still loaded")

  // -------------------------------------------------------- the URL policy
  checkAllowed(imageUrlAllowed("https://example.com/a.png", listOf("*")), true, "a star allows any host")
  checkAllowed(
    imageUrlAllowed("http://example.com/a.png", listOf("*")),
    true, "a star allows http too, and the engine's own policy is what rejects it",
  )
  checkAllowed(imageUrlAllowed("", listOf("*")), true, "a star allows an empty URL, which the state rule then handles")
  checkAllowed(
    imageUrlAllowed("https://example.com/a.png", emptyList()),
    false, "an empty list allows nothing",
  )

  val cdn = listOf("https://cdn.example.com/")
  checkAllowed(imageUrlAllowed("https://cdn.example.com/a.png", cdn), true, "a prefix allows its own subtree")
  checkAllowed(imageUrlAllowed("https://cdn.example.com", cdn), false, "a prefix is not a host")
  checkAllowed(
    imageUrlAllowed("https://evil.example.com/cdn.example.com/a.png", cdn),
    false, "a prefix in the middle of a URL allows nothing",
  )
  checkAllowed(
    imageUrlAllowed("https://cdn.example.com.evil.com/a.png", cdn),
    false, "a host that starts with an allowed one is not allowed",
  )
  checkAllowed(imageUrlAllowed("", cdn), false, "an empty URL matches no prefix")
  checkAllowed(imageUrlAllowed("https://cdn.example.com/", cdn), true, "a prefix matches itself")
  checkAllowed(imageUrlAllowed("HTTPS://CDN.Example.COM/a.png", cdn), true, "a prefix matches ignoring case")
  checkAllowed(
    imageUrlAllowed("https://cdn.example.com/a.png", listOf("HTTPS://CDN.EXAMPLE.COM/")),
    true, "an uppercase prefix matches a lowercase URL",
  )

  val several = listOf("https://images.example.org/", "https://cdn.example.com/")
  checkAllowed(imageUrlAllowed("https://cdn.example.com/a.png", several), true, "the second entry matches")
  checkAllowed(imageUrlAllowed("https://images.example.org/a.png", several), true, "the first entry matches")
  checkAllowed(imageUrlAllowed("https://other.example.net/a.png", several), false, "no entry matches")
  checkAllowed(
    imageUrlAllowed("https://other.example.net/a.png", listOf("https://other.example.net/", "*")),
    true, "the star allows from wherever it sits in the list",
  )
  checkAllowed(imageUrlAllowed("https://example.com/a.png", listOf("")), false, "an empty entry matches nothing")
  checkAllowed(
    imageUrlAllowed("https://example.com/a.png", listOf("", "*")),
    true, "an empty entry does not shadow the star",
  )

  if (failures > 0) {
    println("$failures check(s) failed")
    exitProcess(1)
  }
  println("image layout mirror: every case matches the C++ table")
}