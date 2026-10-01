#pragma once

#include "FenceMeta.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string_view>
#include <vector>

namespace downstream {

/// The code layout inputs that come from props, and so are equal for every block
/// in one view.
struct CodeLayoutProps {
  /// `codeBlockWrap`: laid out at the card's width, so long lines wrap.
  bool wrap{false};
  /// `lineNumbers`. A fence's `noLineNumbers` hides numbers for one block, so
  /// this is the switch and the meta is the per-block exception.
  bool lineNumbers{true};
  /// `codeBlockMaxHeight` in points. 0 means no cap. A float rather than a
  /// double because every measurement downstream is in points as a CGFloat, and
  /// a narrowed value here is one conversion rather than two.
  float maxHeight{400};

  /// A float compares by value here rather than by bits, so a `NaN` height
  /// cannot make two identical props compare different. JavaScript normalizes a
  /// `NaN` height away before it reaches native, so this only guards a direct
  /// caller.
  bool operator==(const CodeLayoutProps &other) const {
    return wrap == other.wrap && lineNumbers == other.lineNumbers &&
        (maxHeight == other.maxHeight || (std::isnan(maxHeight) && std::isnan(other.maxHeight)));
  }
};

/**
 * The reader's own per-block choices, by code ordinal, each sorted.
 *
 * Ordinals are counted as the segment plan is built, so they are stable across
 * append-only snapshots and a choice survives streaming.
 */
struct CodeChoices {
  /// The blocks whose layout differs from `props.wrap`.
  std::vector<uint32_t> wrapToggles;
  /// The blocks whose height cap the expand button lifted.
  std::vector<uint32_t> expanded;

  bool operator==(const CodeChoices &) const = default;
};

/// Everything that decides how code lays out: the props and the reader's choices.
///
/// One value rather than five loose fields, because every input that changes a
/// card's height has to reach the shadow node, the worker, and the view through
/// one definition. Three more fields beside `codeBlockWrap` and `wrapToggles`
/// would repeat the drift the refactor exists to remove.
struct CodeLayout {
  CodeLayoutProps props;
  CodeChoices choices;

  bool operator==(const CodeLayout &) const = default;
};

/// How one code block lays out. The only place props, choices, and meta meet.
struct CodeBlockLayout {
  /// Laid out at the card's width rather than at its own length.
  bool wraps;
  /// `props.lineNumbers && !meta.noLineNumbers`: the prop wins over the meta,
  /// as Streamdown's own `lineNumbers={false}` does over its meta string.
  bool lineNumbers;
  /// The number the gutter draws beside the block's first line.
  uint32_t firstNumber;
  /// The cap this block actually has, in points. 0 when uncapped or expanded,
  /// which is what makes "no cap" one case rather than two.
  float maxHeight;
  /// The reader lifted this block's cap. The card is uncapped either way, but an
  /// expanded one keeps its button, as a collapse button, so the cap can return.
  bool expanded = false;
};

/// The layout of the code block at `ordinal`, from the props, the reader's
/// choices, and that block's own fence meta.
CodeBlockLayout resolveCodeBlock(const CodeLayout &layout, uint32_t ordinal, const FenceMeta &meta);

/// True when `ordinal` is in `values`, which callers keep sorted.
inline bool hasChoice(const std::vector<uint32_t> &values, uint32_t ordinal) {
  return std::binary_search(values.begin(), values.end(), ordinal);
}

/// Adds or removes `ordinal`, keeping the set sorted, and returns whether it is
/// now a member.
inline bool toggleChoice(std::vector<uint32_t> &values, uint32_t ordinal) {
  const auto entry = std::lower_bound(values.begin(), values.end(), ordinal);
  if (entry != values.end() && *entry == ordinal) {
    values.erase(entry);
    return false;
  }
  values.insert(entry, ordinal);
  return true;
}

/**
 * The digits a gutter needs for the widest number it will draw.
 *
 * At least two, so the gutter does not widen when a block crosses line 10, and
 * so Streamdown's "line numbers over 100 wrap and start a new line" cannot
 * happen. `lineCount` is 0 for an empty fence, which still draws one line.
 */
inline uint32_t gutterDigits(uint32_t firstNumber, uint32_t lineCount) {
  // An empty block draws one numbered line, as Streamdown's single empty row
  // does, so the last number is the first one rather than none.
  const uint32_t lastNumber = lineCount <= 1 ? firstNumber : firstNumber + (lineCount - 1);
  uint32_t digits = 1;
  for (uint32_t remaining = lastNumber; remaining >= 10; remaining /= 10) {
    digits++;
  }
  return std::max<uint32_t>(digits, 2);
}

/// The lines a capped block still shows, whatever the font scale.
///
/// A 400-point cap is more than half a phone screen at the default size and
/// would show two lines at the largest Dynamic Type size, which is not a
/// readable block. The floor lives here rather than in either platform, so the
/// number is the one the measurement and the view agree on.
inline constexpr float kMinVisibleLines = 5;

/// The space between the line-number gutter and the code, in points. A constant
/// rather than a theme slot, because it separates two parts of one card rather
/// than styling either of them. The gutter's width includes it, and its numbers
/// end where it begins. Mirrored by `GUTTER_GAP` in CodeLayout.kt.
inline constexpr float kGutterGap = 12;

/**
 * The height of one code card: its header plus `min(body + 2 * padding, cap)`.
 *
 * The cap is on the body box with the card's padding inside it and the header
 * outside, which is the box Streamdown caps. A cap of 0 is no cap, and the floor
 * at `kMinVisibleLines` keeps a capped block readable at accessibility sizes.
 *
 * This is the only place a card's height is decided, and it takes the resolved
 * layout rather than reading props and choices itself, so a caller cannot ask it
 * a question the view answers differently.
 */
inline float codeCardHeight(const CodeBlockLayout &block, float headerHeight, float padding, float bodyHeight,
                            float lineHeight) {
  const float body = bodyHeight + 2 * padding;
  if (!(block.maxHeight > 0)) {
    return headerHeight + body;
  }
  // A cap below the floor would show fewer lines than a reader can read at an
  // accessibility text size, so the floor wins over the cap, not the reverse.
  const float floor = kMinVisibleLines * lineHeight + 2 * padding;
  // A card is never taller than its body: the floor raises a cap that would cut
  // a block below five readable lines, and a body already shorter than that is
  // laid out at its own height.
  return headerHeight + std::min(body, std::max(block.maxHeight, floor));
}

/**
 * The UTF-16 offset of each logical line in a code block's text.
 *
 * Found once per segment text, on the worker, so a gutter's draw pass can walk
 * the lines inside its dirty rect instead of searching the text for newlines
 * every frame. Always at least one entry, so an empty fence still has a first
 * line to number and a draw pass has a line to find.
 */
using CodeLineStarts = std::vector<uint32_t>;

CodeLineStarts codeLineStarts(std::u16string_view text);

} // namespace downstream
