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
#include <cstdint>
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

/// Whether the source ends inside a fenced code block, which is to say a writer
/// has not typed the closing fence yet.
///
/// A line inside an open fence is content rather than a new fence, so this is a
/// walk with one open fence in it rather than a search for the last opener. An
/// indented code block can also continue, but it is not a fence and md4c will
/// not tell a caller which it was, so this answers only for fences.
bool endsInsideFence(std::string_view source);

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

// --- inline tag allowlist ---

/// What an `MD_TEXT_HTML` piece says. `None` keeps the piece literal, as
/// every tag outside the allowlist is; `SubVoid`/`SupVoid` are swallowed,
/// because a writer who means a superscript never writes `<sup/>`.
enum class InlineTag : uint8_t {
  None = 0,
  Break,
  SubOpen,
  SubClose,
  SupOpen,
  SupClose,
  SubVoid,
  SupVoid,
};

/// Classifies a piece md4c already proved was one tag. The name must run out
/// at whitespace, `/`, or `>`, so `<subtitle>` is not `sub`, and attributes
/// are ignored, as the web's sanitize schema drops them. `</br>` is a break,
/// because HTML parsers read it that way and models write it.
InlineTag matchInlineTag(std::string_view piece);

/// True when `fragment` could still grow into one of the allowed tags: `<`
/// alone, or a strict prefix of one of the three names, with no closing `>`
/// yet and ASCII only. Drives the provisional hide of a half-arrived tag at the
/// stream tail; whether hiding it is safe is the caller's to prove.
bool isInlineTagPrefix(std::string_view fragment);

} // namespace downstream::scan
