#pragma once

// Scanners over canonical markdown source, shared by every consumer that has
// to reason about the raw bytes rather than the parsed document.
//
// These exist because the same questions were being answered three times in
// three languages: the parser recovered link destinations with pointer
// arithmetic, the incremental parser re-derived fence grammar per line, and
// the preview pass needs the same answers again. The parser's copy is the one
// with a test suite behind it, so it is the one that moved here first.

#include <cstddef>
#include <optional>
#include <string_view>

namespace downstream::scan {

inline constexpr size_t kNoOffset = std::string_view::npos;

// --- escapes ---

/// True when a backslash immediately before `index` escapes it. An even run
/// of backslashes does not escape, so `\\*` is a real emphasis delimiter.
bool isEscaped(std::string_view source, size_t index);

/// First unescaped `needle` at or after `from`, or kNoOffset.
size_t findUnescaped(std::string_view source, char needle, size_t from);

// --- lines ---

bool isBlankLine(std::string_view line);

/// Offset after at most three leading spaces, or kNoOffset when the line is
/// indented far enough to be code.
size_t afterIndent(std::string_view line);

bool startsWith(std::string_view line, char marker);

/// Whether a character could be part of a word, for the intraword delimiter
/// test. ASCII alphanumerics and underscore are word characters, and so is
/// every non-ASCII unit: treating an unrecognised letter as punctuation would
/// let an opener be treated as intraword-adjacent, and that direction is the
/// unsafe one. Skipping a repair a reader would have liked is a fallback to
/// literal text, which the engine already does correctly.
bool isWordish(char16_t unit);

/// Whitespace as CommonMark defines it for flanking purposes.
bool isMarkdownSpace(char16_t unit);

// --- fences ---

struct Fence {
  char marker;
  size_t length;
  size_t indent;
  std::string_view info;
};

/// An opening fence on this line, if the line opens one. Backticks, tildes,
/// and — when `mathFences` is set — a line that is exactly `$$`.
std::optional<Fence> fenceOpener(std::string_view line, bool mathFences = false);

/// Whether this line closes a fence opened with `marker` repeated `length`
/// times. The marker run must be at least as long and the rest must be blank,
/// so an info string never closes a fence.
bool closesFence(std::string_view line, char marker, size_t length);

/// Whether this line is a fence delimiter of either kind, opening or closing.
bool isFenceLine(std::string_view line);

/// Whether this line is a `$$` display-math fence delimiter: a line that is
/// exactly two dollars, after up to three spaces, with nothing after it.
///
/// Its own predicate rather than a `fenceOpener` call, because the caller is
/// usually asking whether a math block has been closed, and the answer for a
/// backtick fence is irrelevant there. A fence the incremental parser does not
/// recognize is a boundary it will commit inside, which is how a blank line in
/// an `aligned` environment would split a formula.
bool isMathFenceLine(std::string_view line);

// --- inline destinations ---

/// `open` points at '('. Returns the offset after the matching ')', or
/// kNoOffset. Handles nested parens, `<...>` and quoted destinations, and a
/// destination that never closes.
size_t skipInlineDestination(std::string_view source, size_t open);

/// Whether the two bytes before `at` are `](`, which means an autolink found
/// here belongs to a link or image whose target has not been typed yet.
bool isUnfinishedDestination(std::string_view source, size_t at);

} // namespace downstream::scan
