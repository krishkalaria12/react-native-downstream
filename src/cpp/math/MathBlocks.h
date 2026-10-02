#pragma once

// Laying out the formulas in a document, once, on the worker.
//
// Shaped like the highlighting pass on purpose: it runs after the parse, it
// fills the document's own copy of the layouts, and it returns the same handle
// when there is nothing to lay out. A prose-only document therefore never loads
// RaTeX, never reads a font, and never calls the engine — which is the whole of
// what "plain prose does not pay" means for this feature.
//
// The pass never re-runs for a theme change, a rotation, or Dynamic Type,
// because a layout is in em units with the baseline at y = height. It depends on
// the formula's TeX and on nothing else, so it is a function of the document
// rather than of the view.

#include "downstream/Document.h"
#include "math/MathEngine.h"

#include <cstdint>
#include <string>
#include <vector>

namespace downstream {

/// What one set of layout passes did, for the parser's counters. A caller that
/// does not count passes null.
struct MathLayoutStats {
  /// Formulas a pass laid out, including the ones it left literal.
  uint64_t laidOut{0};
  /// Passes that kept a previous snapshot's layout for a formula that has not
  /// finished arriving.
  uint64_t lastGoodKept{0};
  /// Formulas refused before the engine was asked, by the size caps.
  uint64_t skippedTooLarge{0};
  /// Formulas the engine or the decoder rejected.
  uint64_t failed{0};
  /// Passes that continued the previous one rather than starting over.
  uint64_t resumes{0};
};

namespace math_limits {

/// The most TeX one formula may carry, in bytes.
///
/// A bound on hostile input, applied before anything reaches a library that
/// aborts the process on a panic. A real formula from a model is a few hundred
/// bytes; 4 KiB is a formula longer than any reader wants and shorter than
/// anything a document legitimately contains.
inline constexpr size_t kMaxTexBytes = 4096;

/// The most formulas one document may carry.
///
/// Also applied before the engine, and for the same reason. A document at this
/// size is a model that was asked for a table, not a chat answer.
inline constexpr size_t kMaxFormulas = 512;

} // namespace math_limits

/**
 * Lays out every formula in `document` and returns a handle carrying the
 * results.
 *
 * Returns `document` itself when its tail has no formula, so a document without
 * math allocates nothing and calls no engine.
 *
 * `previous` is the last snapshot, whose layouts are the answer for a formula
 * that is still arriving. A formula whose block is unsettled and that fails to
 * lay out keeps the previous layout of the same span when that span's TeX is a
 * prefix of the new TeX: while `\frac{a}{` streams in, the reader keeps seeing
 * the last prefix that laid out rather than a formula that flickers to its
 * source on every token.
 *
 * Never throws. A formula that cannot be laid out records its message and draws
 * as its literal, which is what KaTeX's `throwOnError: false` does.
 */
DocumentHandle layoutMath(DocumentHandle document, const DocumentHandle &previous = {},
                          MathLayoutStats *stats = nullptr);

/// Lays out the formulas in a mutable block, for a caller that already holds
/// the tail. Returns the number of formulas it laid out.
///
/// A block passed here is treated as settled, so a formula that fails records
/// its error. Use `layoutMathTail` for a block that may still grow.
uint64_t layoutMathInBlock(Block *block, MathLayoutStats *stats = nullptr);

/// Lays out a stream's unstable tail, in place.
///
/// Every block but the last math block is settled, because a later block means
/// the earlier one can no longer change. The last one may still grow, so its
/// formulas are laid out as unsettled: one that does not parse keeps the
/// previous snapshot's layout rather than flickering to its source on every
/// token. This is the same pass `layoutMath` runs, with the difference that it
/// mutates the tail a snapshot already owns rather than making a copy of it.
void layoutMathTail(std::vector<Block> *tail, const Document *previous, MathLayoutStats *stats);

/// The formulas in a document, as the engine counts them. What the caps are
/// applied to, and what a test asserts against.
uint64_t countFormulas(const Document &document);

} // namespace downstream
