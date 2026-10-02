#pragma once

// Rewrites `\(…\)` and `\[…\]` to `$$…$$` in place, so md4c can recognize them.
//
// md4c knows about `$` and nothing else, and adding a third delimiter pair to it
// would be a patch upstream has no reason to accept. This pass runs before the
// parse, on a copy of the source, and rewrites each two-byte delimiter to `$$`.
//
// The copy has exactly the same length as the original, so every offset md4c
// reports still points at the same byte of the canonical source. That is the
// whole reason this is a rewrite rather than a translation table: the builder
// reads the original bytes at md4c's offsets, and therefore code, link text and
// an unpaired delimiter show exactly what the writer sent.
//
// A delimiter is only rewritten when it pairs, is unescaped, and is not inside a
// code span or a code fence. An unpaired `\(` is left alone, and CommonMark then
// reads it as the escape it is, so a reader who meant a literal parenthesis
// still gets one.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace downstream::scan {

/// One rewritten delimiter pair, as byte offsets in the source it was scanned.
struct MathPair {
  /// Offset of the opening two bytes.
  size_t open{0};
  /// Offset after the closing two bytes.
  size_t close{0};
  /// False for `\(…\)`, true for `\[…\]`.
  bool display{false};
  /// True when the opening delimiter was alone on its line and the closing one
  /// is too, which is what makes a rewritten `\[` a fence line rather than a
  /// span. Decided here because by the time the builder sees md4c's report the
  /// two have become indistinguishable.
  bool fenceLine{false};
};

/**
 * A source with its bracket delimiters rewritten, or the source itself when
 * there was nothing to rewrite.
 *
 * The same length either way, which is the invariant everything downstream
 * depends on. `pairs` is sorted by `open` and non-overlapping.
 */
struct MathSource {
  /// The bytes to hand to md4c. Owned here, or a view of the caller's source
  /// when no rewrite was needed.
  std::string text;
  /// The pairs that were rewritten.
  std::vector<MathPair> pairs;
  /// The source, so a caller that wants to free the copy can.
  std::string_view original;

  bool empty() const { return pairs.empty(); }
};

/**
 * Finds the bracket math in `source` and rewrites it to `$$`.
 *
 * Never throws and never reallocates for a source with no brackets, so a
 * prose-only document pays one scan and nothing else.
 *
 * Code is skipped, and skipped for the same reason md4c resolves it first among
 * verbatim spans: a `$` or `\[` inside backticks is the characters the reader
 * typed. A fence is tracked the same way the incremental parser tracks one, so a
 * bracket inside a code block is not math even though the block's contents are
 * never examined for spans.
 */
MathSource rewriteMathDelimiters(std::string_view source, bool brackets);

} // namespace downstream::scan
