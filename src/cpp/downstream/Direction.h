#pragma once

#include "BidiTable.h"
#include "Document.h"

#include <cstdint>

namespace downstream {

/**
 * The layout direction of one block, decided once.
 *
 * A block reads the way the majority of its strong characters read, and on a tie
 * the way its first strong character does. The first wins the tie because a
 * reader's eye starts at the beginning of the line: a line whose first word is
 * Arabic and whose second is English is a line that opens right to left, whatever
 * the count says.
 *
 * Two kinds of block are skipped rather than counted:
 *
 * - **Code and math.** A fenced block is already laid out left to right on both
 *   platforms, as a code block is everywhere, and a formula is a drawing rather
 *   than a sentence. Counting the words inside `x` would decide a code card's
 *   direction from the identifier a programmer happened to choose.
 * - **A block with no strong character at all.** A table of numbers, a rule, a
 *   line of punctuation. There is nothing to count, so the answer is `Inherit`
 *   and the block takes the one before it, which is what a reader expects from a
 *   line that is only a continuation of what came above.
 *
 * This is the one place the rule lives. Both platforms used to decide separately
 * — iOS from a hand-written range list, Android from
 * `Character.getDirectionality` — and a paragraph that opened with `` `x` `` and
 * then Arabic came out two different ways.
 */
BlockDirection blockDirection(const Block &block, BlockDirection inherited);

/// The same rule over UTF-16, so a caller with the text rather than a block
/// answers the same question.
BlockDirection textDirection(std::u16string_view text, BlockDirection inherited);

} // namespace downstream