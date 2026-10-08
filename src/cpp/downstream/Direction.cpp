#include "Direction.h"

namespace downstream {

namespace {

/// The code point at `index`, joining a surrogate pair into the value the
/// direction table is indexed by. Advances `index` past the pair when there is
/// one, so a supplementary character is counted once rather than twice.
uint32_t codePointAt(std::u16string_view text, size_t &index) {
  const uint32_t unit = text[index];
  if (unit >= 0xd800 && unit <= 0xdbff && index + 1 < text.size()) {
    const uint32_t trail = text[index + 1];
    if (trail >= 0xdc00 && trail <= 0xdfff) {
      index++;
      return 0x10000 + ((unit - 0xd800) << 10) + (trail - 0xdc00);
    }
  }
  return unit;
}

} // namespace

BlockDirection textDirection(std::u16string_view text, BlockDirection inherited) {
  uint32_t left = 0;
  uint32_t right = 0;
  // The first strong character is kept separately because it settles a tie, and
  // a tie is not rare: a line of an English sentence with one Arabic word in it
  // is two counts against three and reads left to right, and the same line with
  // two Arabic words reads the other way.
  BlockDirection first = BlockDirection::Inherit;

  for (size_t index = 0; index < text.size(); index++) {
    const uint32_t unit = text[index];
    if (unit < 0x80) {
      // The only strong characters below U+0080 are the ASCII letters: digits
      // are weak, the rest is neutral, and no right-to-left character lives
      // down here. Reading them inline keeps an English paragraph out of the
      // range table entirely, which is where a long document spends its parse.
      if ((unit >= 'A' && unit <= 'Z') || (unit >= 'a' && unit <= 'z')) {
        left++;
        if (first == BlockDirection::Inherit) {
          first = BlockDirection::LeftToRight;
        }
      }
      continue;
    }
    const uint32_t code = codePointAt(text, index);
    if (isStrongRightToLeft(code)) {
      right++;
      if (first == BlockDirection::Inherit) {
        first = BlockDirection::RightToLeft;
      }
    } else if (isStrongLeftToRight(code)) {
      left++;
      if (first == BlockDirection::Inherit) {
        first = BlockDirection::LeftToRight;
      }
    }
  }

  if (first == BlockDirection::Inherit) {
    // No strong character at all: a table of numbers, a rule, a line of
    // punctuation. There is nothing to count, so the block inherits rather than
    // being told to read left to right because that is where most of the world
    // starts.
    return inherited;
  }
  if (left == right) {
    return first;
  }
  return right > left ? BlockDirection::RightToLeft : BlockDirection::LeftToRight;
}

BlockDirection blockDirection(const Block &block, BlockDirection inherited) {
  // Code and math are skipped rather than counted: a fenced block is laid out
  // left to right on both platforms as a code block is everywhere, and a formula
  // is a drawing rather than a sentence. Counting the words inside an identifier
  // would decide a code card's direction from the name a programmer happened to
  // choose.
  if (block.kind == BlockKind::CodeBlock || block.kind == BlockKind::MathBlock) {
    return BlockDirection::Inherit;
  }
  return textDirection(block.text, inherited);
}

} // namespace downstream