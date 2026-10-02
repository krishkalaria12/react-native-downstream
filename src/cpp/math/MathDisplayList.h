#pragma once

// Reading RaTeX's display list JSON.
//
// A small reader for one schema rather than a JSON library, for three reasons.
// The input is from a pinned binary we ship, not from a network. The decoder
// has to be tolerant of unknown fields and item types because the protocol says
// so and because RaTeX is pre-1.0. And a list is bounded data — a few thousand
// items, a nesting depth, a width in em — so the reader can enforce those
// bounds in one pass and refuse a list that claims something impossible, which
// is the only way a malformed list reaches a platform's drawing code.
//
// A refused list is a failed formula, never a crash. The caller shows the
// formula's source, which is what KaTeX does with `throwOnError: false`.

#include "MathLayout.h"

#include <string_view>

namespace downstream::math {

/// The limits a display list has to stay inside. A formula that exceeds one has
/// failed, and refusing it is cheaper and safer than drawing it.
namespace limits {

/// A width over this is not a formula, it is a `\\rule{1e6em}{1e6em}`.
inline constexpr float kMaxWidthEm = 2000.0f;
/// An item count over this is the same kind of attack.
inline constexpr size_t kMaxItems = 20000;
/// A path with more commands than this is a stroked-out rectangle, not a glyph.
inline constexpr size_t kMaxPathCommands = 4096;
/// Nesting depth for arrays and objects. The schema nests two levels; anything
/// deeper is a document built to exhaust a stack.
inline constexpr size_t kMaxDepth = 32;
/// A code point outside the Unicode range, or a surrogate, is a malformed item.
inline constexpr char32_t kMaxCodePoint = 0x10FFFF;

} // namespace limits

/// Decodes one display list.
///
/// `out` is filled only on success. Returns false and sets `error` for anything
/// the protocol does not allow: malformed JSON, a non-finite number, a width
/// over the limit, more items than the limit, a nesting depth over the limit,
/// or a code point that is not one.
bool decodeDisplayList(std::string_view json, MathLayout *out, std::string *error);

/// The font a name maps to, or `MathFont::System` for a name this build does
/// not know. System rather than a failure, so a RaTeX version that adds a font
/// renders with the platform's rather than losing the glyphs.
MathFont fontFromName(std::string_view name);

/// The file name of a font inside the package's `fonts` directory.
std::string_view fileNameForFont(MathFont font);

/// The PostScript name a platform registers or loads a face by.
std::string_view postScriptNameForFont(MathFont font);

} // namespace downstream::math
