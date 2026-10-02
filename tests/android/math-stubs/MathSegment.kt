package com.downstream

import android.graphics.drawable.GradientDrawable
import android.view.View

// Android exposes background as a Java synthetic property. An extension keeps
// the host Kotlin stub from hiding MathSegmentView's private background child.
internal var View.background: GradientDrawable?
  get() = recordedBackground
  set(value) { recordedBackground = value }

// The fields read by MathSegmentView, without RenderedDocument's RN dependencies.
internal class MathSegment(
  val literal: String,
  val height: Float,
  val depth: Float,
  val resolved: Boolean,
  val displayList: MathDisplayList?,
  val failed: Boolean,
  val speech: String,
  val textAlign: Int,
)
