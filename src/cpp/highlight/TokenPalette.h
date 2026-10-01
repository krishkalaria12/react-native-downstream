#pragma once

#include "Document.h"

#include <cstddef>
#include <cstdint>

namespace downstream {

/// GitHub's Primer "prettylights" colors, the palette behind Shiki's
/// `github-light-default` and `github-dark-default` themes. Both platforms read
/// this one table, so a token class cannot drift between iOS and Android.
///
/// Values are 0xAARRGGBB, in `TokenClass` order. Plain and Operator are the
/// code text color, as punctuation is in GitHub's themes, so a platform needs no
/// entry of its own for either.
struct TokenColors {
  uint32_t light;
  uint32_t dark;
};

inline constexpr TokenColors kTokenColors[static_cast<size_t>(TokenClass::Count)] = {
    {0xff1f2328, 0xfff0f6fc}, // Plain
    {0xff6e7781, 0xff8b949e}, // Comment
    {0xffcf222e, 0xffff7b72}, // Keyword
    {0xff8250df, 0xffd2a8ff}, // Type
    {0xff0a3069, 0xffa5d6ff}, // String
    {0xff0550ae, 0xff79c0ff}, // Number
    {0xff0550ae, 0xff79c0ff}, // Constant
    {0xff116329, 0xff7ee787}, // Regex
    {0xff8250df, 0xffd2a8ff}, // Function
    {0xff0550ae, 0xff79c0ff}, // Property
    {0xff1f2328, 0xfff0f6fc}, // Operator
    {0xff953800, 0xffffa657}, // Preprocessor
    {0xff116329, 0xff7ee787}, // Tag
    {0xff0550ae, 0xff79c0ff}, // Attribute
    {0xff116329, 0xff7ee787}, // Inserted
    {0xff82071e, 0xffffa198}, // Deleted
    {0xff0550ae, 0xff79c0ff}, // Heading
};

inline constexpr size_t kTokenColorsCount = static_cast<size_t>(TokenClass::Count);

/// The color for one token class in one appearance. The default text color
/// stands in for the classes that carry no color of their own.
inline uint32_t tokenColor(TokenClass token, bool dark) {
  const TokenColors &colors = kTokenColors[static_cast<size_t>(token)];
  return dark ? colors.dark : colors.light;
}

} // namespace downstream