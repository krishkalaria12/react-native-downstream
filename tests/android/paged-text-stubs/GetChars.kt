package android.text

// Host contract only; actual GetChars/SpannableStringBuilder is verified on ART.
interface GetChars : CharSequence {
  fun getChars(start: Int, end: Int, dest: CharArray, destoff: Int)
}
