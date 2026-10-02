package com.downstream

import android.content.Context
import android.content.res.AssetManager
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.Path
import android.graphics.Typeface
import android.text.style.ReplacementSpan
import java.util.concurrent.ConcurrentHashMap
import kotlin.math.ceil

/**
 * One formula's display list, as the engine laid it out.
 *
 * Decoded from the doubles `appendDisplayList` writes in `DownstreamJni.cpp`, in
 * em units with the origin at the top-left of the formula's box and the baseline
 * at `y = height`, which is RaTeX's own coordinate system. Nothing here knows a
 * size or a color: [MathRenderer] multiplies by the size it is asked for and
 * fills `kInheritColor` items with the run's own color, so a theme or a dark-mode
 * switch reaches a formula without laying it out again.
 */
internal class MathDisplayList(
  val width: Float,
  val height: Float,
  val depth: Float,
  val glyphs: List<Glyph>,
  val rules: List<Rule>,
  val paths: List<Shape>,
) {
  class Glyph(val x: Float, val y: Float, val scale: Float, val font: Int, val codePoint: Int, val color: Int) {
    // The display list is immutable; encode supplementary code points only once.
    val text: String? = if (codePoint > 0 && Character.isValidCodePoint(codePoint)) {
      String(Character.toChars(codePoint))
    } else {
      null
    }
  }

  /** A filled box, top-left and size: a fraction bar, a `\rule`, a colorbox. */
  class Rule(val x: Float, val y: Float, val width: Float, val height: Float, val color: Int, val dashed: Boolean)

  class Shape(
    val x: Float,
    val y: Float,
    val fill: Boolean,
    val strokeWidth: Float,
    val color: Int,
    /** Seven floats per command: op, x, y, x1, y1, x2, y2. */
    commands: FloatArray,
  ) {
    // Keep geometry in em. Canvas scaling applies the current size on every draw.
    val path: Path = pathOf(commands)
  }

  companion object {
    /** `kInheritColor`: draw in the surrounding text's color. */
    const val INHERIT = 0

    private const val OP_MOVE = 0
    private const val OP_LINE = 1
    private const val OP_QUAD = 2
    private const val OP_CUBIC = 3
    private const val OP_CLOSE = 4

    /**
     * Reads one list starting at [offset], or null when the numbers run out.
     *
     * A short or malformed table is a formula that shows its literal rather than an
     * exception, because a native library and this file built from two versions
     * must degrade the way the engine's own decoder does.
     */
    fun decode(values: DoubleArray, offset: Int, length: Int): MathDisplayList? {
      val end = offset + length
      var at = offset
      fun next(): Double? = if (at < end && at < values.size) values[at++] else null
      fun nextFloat(): Float? = next()?.toFloat()
      fun nextInt(): Int? = next()?.toLong()?.toInt()
      return try {
        val width = nextFloat() ?: return null
        val height = nextFloat() ?: return null
        val depth = nextFloat() ?: return null
        val glyphs = List(nextInt() ?: return null) {
          Glyph(nextFloat()!!, nextFloat()!!, nextFloat()!!, nextInt()!!, nextInt()!!, nextInt()!!)
        }
        val rules = List(nextInt() ?: return null) {
          Rule(nextFloat()!!, nextFloat()!!, nextFloat()!!, nextFloat()!!, nextInt()!!, next()!! != 0.0)
        }
        val paths = List(nextInt() ?: return null) {
          val x = nextFloat()!!
          val y = nextFloat()!!
          val fill = next()!! != 0.0
          val stroke = nextFloat()!!
          val color = nextInt()!!
          val count = nextInt()!!
          val commands = FloatArray(count * 7) { nextFloat()!! }
          Shape(x, y, fill, stroke, color, commands)
        }
        MathDisplayList(width, height, depth, glyphs, rules, paths)
      } catch (error: NullPointerException) {
        null
      }
    }

    /** Builds an Android path from seven-float commands, in em. */
    internal fun pathOf(commands: FloatArray): Path {
      val path = Path()
      var index = 0
      while (index + 7 <= commands.size) {
        val x = commands[index + 1]
        val y = commands[index + 2]
        val x1 = commands[index + 3]
        val y1 = commands[index + 4]
        val x2 = commands[index + 5]
        val y2 = commands[index + 6]
        when (commands[index].toInt()) {
          OP_MOVE -> path.moveTo(x, y)
          OP_LINE -> path.lineTo(x, y)
          OP_QUAD -> path.quadTo(x1, y1, x, y)
          OP_CUBIC -> path.cubicTo(x1, y1, x2, y2, x, y)
          OP_CLOSE -> path.close()
        }
        index += 7
      }
      return path
    }
  }
}

/**
 * The KaTeX faces, loaded from the math package's assets on first use.
 *
 * `Typeface.createFromAsset` rather than a font family registered with the app,
 * so another library's copy of KaTeX_Main cannot collide with this one by name.
 * The file order is `MathFont`'s, which `mathFontFileName` in `MathLayout.cpp`
 * also follows.
 */
internal object MathFonts {
  private val files = arrayOf(
    "KaTeX_Main-Regular.ttf",
    "KaTeX_Main-Bold.ttf",
    "KaTeX_Main-Italic.ttf",
    "KaTeX_Main-BoldItalic.ttf",
    "KaTeX_Math-Italic.ttf",
    "KaTeX_Math-BoldItalic.ttf",
    "KaTeX_AMS-Regular.ttf",
    "KaTeX_Caligraphic-Regular.ttf",
    "KaTeX_Caligraphic-Bold.ttf",
    "KaTeX_Fraktur-Regular.ttf",
    "KaTeX_Fraktur-Bold.ttf",
    "KaTeX_SansSerif-Regular.ttf",
    "KaTeX_SansSerif-Bold.ttf",
    "KaTeX_SansSerif-Italic.ttf",
    "KaTeX_Script-Regular.ttf",
    "KaTeX_Typewriter-Regular.ttf",
    "KaTeX_Size1-Regular.ttf",
    "KaTeX_Size2-Regular.ttf",
    "KaTeX_Size3-Regular.ttf",
    "KaTeX_Size4-Regular.ttf",
  )

  /** The asset directory the math package registered, which is RaTeX's own. */
  private const val DIRECTORY = "fonts"

  private val loaded = ConcurrentHashMap<Int, Typeface>()
  private val missing = ConcurrentHashMap.newKeySet<Int>()

  /** The app's assets, where the math package's AAR puts the fonts. */
  @Volatile
  private var assets: AssetManager? = null

  /** Remembers the app's assets, once. Any context of the app will do. */
  fun attach(context: Context) {
    if (assets == null) {
      assets = context.applicationContext.assets
    }
  }

  /**
   * The face for [font], or the platform's default for `System` and for a face
   * that is not there: a code point KaTeX has no glyph for, such as CJK or an
   * emoji, draws in the platform font, which is what RaTeX asks for.
   */
  fun typeface(font: Int): Typeface {
    val assets = assets
    if (assets == null || font < 0 || font >= files.size || font in missing) {
      return Typeface.DEFAULT
    }
    loaded[font]?.let { return it }
    return try {
      Typeface.createFromAsset(assets, "$DIRECTORY/${files[font]}").also { loaded[font] = it }
    } catch (error: RuntimeException) {
      missing.add(font)
      Typeface.DEFAULT
    }
  }
}

/**
 * Draws a [MathDisplayList] into a canvas, the Android half of `DSMathRenderer`.
 *
 * The canvas origin is the top-left of the formula's box. Vectors at draw time,
 * so a formula is sharp at any size without being laid out again.
 */
internal class MathRenderer {
  private val fill = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.FILL }
  private val stroke = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE }
  private val text = Paint(Paint.ANTI_ALIAS_FLAG or Paint.SUBPIXEL_TEXT_FLAG)

  /**
   * Draws [list] at [size] pixels per em, with [color] for every item the formula
   * did not color itself. [minimumRule] is the thinnest a rule may be, in pixels,
   * so a fraction bar at a small size stays visible as RaTeX's own renderers keep it.
   */
  fun draw(canvas: Canvas, list: MathDisplayList, size: Float, color: Int, minimumRule: Float) {
    if (size <= 0f) {
      return
    }
    fun ink(value: Int) = if (value == MathDisplayList.INHERIT) color else value

    for (rule in list.rules) {
      fill.color = ink(rule.color)
      val left = rule.x * size
      val right = (rule.x + rule.width) * size
      var top = rule.y * size
      var height = rule.height * size
      if (height < minimumRule) {
        top -= (minimumRule - height) / 2
        height = minimumRule
      }
      if (rule.dashed) {
        val dash = height * 3
        var x = left
        while (x < right) {
          canvas.drawRect(x, top, minOf(x + dash, right), top + height, fill)
          x += dash * 2
        }
      } else {
        canvas.drawRect(left, top, right, top + height, fill)
      }
    }

    for (shape in list.paths) {
      canvas.save()
      canvas.translate(shape.x * size, shape.y * size)
      canvas.scale(size, size)
      val paint = if (shape.fill) fill else stroke
      paint.color = ink(shape.color)
      if (!shape.fill) {
        // In em, because the canvas is scaled to em here.
        stroke.strokeWidth = if (shape.strokeWidth > 0) shape.strokeWidth else minimumRule / size
      }
      canvas.drawPath(shape.path, paint)
      canvas.restore()
    }

    for (glyph in list.glyphs) {
      val glyphText = glyph.text ?: continue
      text.typeface = MathFonts.typeface(glyph.font)
      text.textSize = size * glyph.scale
      text.color = ink(glyph.color)
      canvas.drawText(glyphText, glyph.x * size, glyph.y * size, text)
    }
  }
}

/**
 * One inline formula, drawn in the line it sits in.
 *
 * A replacement span rather than replaced characters, so the literal stays the
 * text: a copy writes the writer's own source, a link around the formula still
 * resolves by offset, and the engine's UTF-16 offsets keep meaning what they say.
 * It draws at the run's own size times KaTeX's 1.21 and in the run's own color,
 * which is the paint it is handed, and raises the line to fit a tall formula.
 */
internal class MathInlineSpan(private val list: MathDisplayList, private val minimumRule: Float) : ReplacementSpan() {
  private val renderer = MathRenderer()

  override fun getSize(paint: Paint, text: CharSequence?, start: Int, end: Int, fm: Paint.FontMetricsInt?): Int {
    val size = paint.textSize * kInlineMathScale
    if (fm != null) {
      val own = paint.fontMetricsInt
      fm.ascent = minOf(own.ascent, -ceil(list.height * size).toInt())
      fm.descent = maxOf(own.descent, ceil(list.depth * size).toInt())
      fm.top = minOf(own.top, fm.ascent)
      fm.bottom = maxOf(own.bottom, fm.descent)
      fm.leading = own.leading
    }
    return ceil(list.width * size).toInt()
  }

  override fun draw(
    canvas: Canvas,
    text: CharSequence?,
    start: Int,
    end: Int,
    x: Float,
    top: Int,
    y: Int,
    bottom: Int,
    paint: Paint,
  ) {
    val size = paint.textSize * kInlineMathScale
    canvas.save()
    // `y` is the line's baseline and the formula's baseline is `height` below its top.
    canvas.translate(x, y - list.height * size)
    renderer.draw(canvas, list, size, paint.color, minimumRule)
    canvas.restore()
  }
}
