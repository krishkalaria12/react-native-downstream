#pragma once

#include <cstdint>

namespace downstream {

/// Unicode identifier classes, for the lexers that ask the host about a
/// non-ASCII code point. Generated tables in IdentifierClasses.cpp.
bool isIdentifierStart(uint32_t point);
/// ID_Continue, plus ZWNJ and ZWJ, which ECMAScript also allows.
bool isIdentifierContinue(uint32_t point);

} // namespace downstream
