#pragma once

#include "Document.h"

#include <cstddef>
#include <cstdint>

namespace downstream {

/// GitHub's Primer "prettylights" colors, the palette behind Shiki's
/// `github-light` and `github-dark` themes, which are Streamdown's defaults.
/// Both platforms read this one table, so a token class cannot drift between
/// iOS and Android.
///
/// Values are 0xAARRGGBB. `kTokenColors[0]` is the code text color for
/// Operator and Plain, so those need no entry of their own.
struct TokenColors {
  uint32_t light;
  uint32_t dark;
};

inline constexpr TokenColors kTokenColors[static_cast<size_t>(TokenClass::Count)] = {
    [static_cast<size_t>(TokenClass::Plain)] = {0xff1f2328, 0xfff0f6fc},
    [static_cast<size_t>(TokenClass::Comment)] = {0xff6e7781, 0xff8b949e},
    [static_cast<size_t>(TokenClass::Keyword)] = {0xffcf222e, 0xffff7b72},
    [static_cast<size_t>(TokenClass::Type)] = {0xff8250df, 0xffd2a8ff},
    [static_cast<size_t>(TokenClass::String)] = {0xff0a3069, 0xffa5d6ff},
    [static_cast<size_t>(TokenClass::Number)] = {0xff0550ae, 0xff79c0ff},
    [static_cast<size_t>(TokenClass::Constant)] = {0xff0550ae, 0xff79c0ff},
    [static_cast<size_t>(TokenClass::Regex)] = {0xff116329, 0xff7ee787},
    [static_cast<size_t>(TokenClass::Function)] = {0xff8250df, 0xffd2a8ff},
    [static_cast<size_t>(TokenClass::Property)] = {0xff0550ae, 0xff79c0ff},
    // Punctuation stays the code text color, as in GitHub's themes.
    [static_cast<size_t>(TokenClass::Operator)] = {0xff1f2328, 0xfff0f6fc},
    [static_cast<size_t>(TokenClass::Preprocessor)] = {0xff953800, 0xffffa657},
    [static_cast<size_t>(TokenClass::Tag)] = {0xff116329, 0xff7ee787},
    [static_cast<size_t>(TokenClass::Attribute)] = {0xff0550ae, 0xff79c0ff},
    [static_cast<size_t>(TokenClass::Inserted)] = {0xff116329, 0xff7ee787},
    [static_cast<size_t>(TokenClass::Deleted)] = {0xff82071e, 0xffffa198},
    [static_cast<size_t>(TokenClass::Heading)] = {0xff0550ae, 0xff79c0ff},
};

inline constexpr size_t kTokenColorsCount = static_cast<size_t>(TokenClass::Count);

/// The color for one token class in one appearance. The default text color
/// stands in for the classes that carry no color of their own.
inline uint32_t tokenColor(TokenClass token, bool dark) {
  const TokenColors &colors = kTokenColors[static_cast<size_t>(token)];
  return dark ? colors.dark : colors.light;
}

} // namespace downstream
