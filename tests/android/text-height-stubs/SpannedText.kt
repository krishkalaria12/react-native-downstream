package com.downstream

import android.text.Spanned

internal class SpannedText(private val value: String) : Spanned, CharSequence by value
