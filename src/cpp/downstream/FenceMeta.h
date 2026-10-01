#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace downstream {

/// One highlighted range of logical lines, `{3-5}` in a fence's info string.
///
/// Parsed so a mark can never reach a title, and carried so the renderer that
/// draws them does not have to parse the info string a second time. Nothing
/// draws them yet.
struct LineMark {
  /// First and last line, one-based and inclusive. `first <= last`.
  uint32_t first{0};
  uint32_t last{0};

  bool operator==(const LineMark &other) const = default;
};

/// What a code fence's info string says about how to present it.
///
/// Parsed once, where md4c reports the block, so the header label, the gutter,
/// and the download name all read one struct rather than each scanning the raw
/// string their own way. See `docs/code-block-chrome-plan.html` for the grammar.
struct FenceMeta {
  /// From `title="…"`, `filename="…"`, or the path after `lang:` in the first
  /// word. UTF-8 and sanitized: control and bidi characters removed, at most
  /// `kMaxTitleUnits` UTF-16 units. Empty when the fence named nothing.
  std::string title;
  /// The first line number the gutter draws, from `startLine=N`,
  /// `showLineNumbers=N`, or `showLineNumbers{N}`. Values below 1 and above
  /// `kMaxStartLine` are ignored, so a hostile value cannot make the gutter
  /// absurdly wide.
  uint32_t startLine{1};
  /// The fence asked for no line numbers, with `noLineNumbers`.
  bool noLineNumbers{false};
  /// `{1,3-5}`, sorted, at most `kMaxLineMarks`.
  std::vector<LineMark> marks;

  bool operator==(const FenceMeta &) const = default;
};

/// At most this many UTF-16 units of title, which is long enough for a path and
/// short enough for a 32-point header row.
inline constexpr size_t kMaxTitleUnits = 96;

/// The largest line number a fence may name. A start line past this would make
/// a gutter thousands of digits wide for one code line.
inline constexpr uint32_t kMaxStartLine = 1'000'000;

/// How many marks one fence may carry. A chat model rarely writes any, and an
/// unbounded list would let a short info string allocate an arbitrary one.
inline constexpr size_t kMaxLineMarks = 64;

/// At most this much of an info string is read. The whole string is kept in
/// `Block::info`; this only bounds the parse.
inline constexpr size_t kMaxInfoBytes = 1024;

/**
 * The meta a fence's info string carries, or an empty meta.
 *
 * Total: every input parses, and a partial input yields nothing it cannot
 * finish. An unterminated quote or brace drops that token and everything after
 * it, so a fence whose info string is still being typed shows no title rather
 * than a wrong one. Tokens this does not recognize are ignored, and the raw
 * string stays available in `Block::info`.
 */
FenceMeta parseFenceMeta(std::string_view info);

} // namespace downstream
