#pragma once

#include <cstdint>

namespace downstream {

/**
 * The strong-direction ranges, from `generate-bidi-table.mjs`.
 *
 * Only the two strong classes are here, because the layout direction rule only
 * counts strong characters: a number, a punctuation mark and a combining mark are
 * all neutral to it, and a table of them would be a third of the size answering
 * no question this asks. Everything else about a code point — where it wraps, how
 * it is reordered — is the platform text system's job, and the two platforms
 * already agree on that part because they both take the engine's direction rather
 * than deciding one of their own.
 */
bool isStrongLeftToRight(uint32_t codePoint);
bool isStrongRightToLeft(uint32_t codePoint);

} // namespace downstream