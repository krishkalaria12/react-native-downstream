#pragma once

#include "RenderPreparer.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace downstream {

/// Bits of the native `controls` prop, in the order `src/controls.ts` sets
/// them. A platform builds its header and its toolbar from these and nothing
/// else, so what a button exists for is decided in one place.
namespace ControlBits {

constexpr uint32_t CodeCopy = 1u << 0;
constexpr uint32_t CodeDownload = 1u << 1;
constexpr uint32_t TableCopy = 1u << 2;
constexpr uint32_t TableDownload = 1u << 3;
constexpr uint32_t CodeWrap = 1u << 4;
} // namespace ControlBits

/// True when any code control is on, so a code block gets a header.
constexpr bool hasCodeControls(uint32_t controls) {
  return (controls & (ControlBits::CodeCopy | ControlBits::CodeDownload | ControlBits::CodeWrap)) != 0;
}

/// True when any table control is on, so a table gets a toolbar.
constexpr bool hasTableControls(uint32_t controls) {
  return (controls & (ControlBits::TableCopy | ControlBits::TableDownload)) != 0;
}

/// The layout of the code block at `ordinal`: laid out at the card's width, so
/// long lines wrap, or laid out at its own length, so they scroll sideways.
///
/// `wrapToggles` holds the ordinals whose layout differs from `codeBlockWrap`,
/// so the choice is one comparison rather than a per-block rule. This is the
/// single definition of the decision: the shadow node that measures, the worker
/// that prepares, and the view that draws all call it, which is what keeps a
/// block's measured height and its drawn height the same number.
inline bool codeWraps(bool codeBlockWrap, const std::vector<uint32_t> &wrapToggles, uint32_t ordinal) {
  const bool toggled =
      std::binary_search(wrapToggles.begin(), wrapToggles.end(), ordinal);
  return toggled != codeBlockWrap;
}

/**
 * The chrome a document lays out with, from the `controls` prop, the
 * `codeBlockWrap` prop, and the reader's own wrap choices.
 *
 * A free function rather than a member so the shadow node that measures, the
 * view that lays out, and the worker that prepares cannot disagree about a
 * height. A header and a toolbar are fixed rows, so they are bits. Wrapping is
 * not a bit: a document where the reader has toggled any block has code at two
 * different heights, and no single bit says which. `kChromeCodeWrap` therefore
 * means "every code block in this document wraps", and it is a *cache key*, not
 * the layout itself; `codeWraps` above decides any one block, and it needs
 * `codeBlockWrap` and the toggle set, which is why those two travel beside it.
 */
inline uint32_t chromeForProps(uint32_t controls, bool codeBlockWrap, const std::vector<uint32_t> &wrapToggles) {
  uint32_t chrome = kChromeNone;
  if (hasCodeControls(controls)) {
    chrome |= kChromeCodeHeader;
  }
  if (hasTableControls(controls)) {
    chrome |= kChromeTableToolbar;
  }
  if ((controls & ControlBits::CodeWrap) != 0 && codeBlockWrap && wrapToggles.empty()) {
    chrome |= kChromeCodeWrap;
  }
  return chrome;
}

} // namespace downstream
