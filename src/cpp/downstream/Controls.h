#pragma once

#include "CodeLayout.h"
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
constexpr uint32_t CodeExpand = 1u << 5;
/// A loaded image's save button. No chrome bit goes with it: the button is
/// drawn over the image rather than in a row above it, so turning it on changes
/// no block's height and therefore nothing about a document's layout.
constexpr uint32_t ImageDownload = 1u << 6;
/// A table's expand button, which lifts `tableMaxHeight` for one table. It
/// changes a table's height only when that table is taller than the cap, so it
/// joins no chrome bit either — `resolveTable` gives that number per table.
constexpr uint32_t TableExpand = 1u << 7;
/// A table's full-screen button. It opens a modal holding a second view, so like
/// the image's download button it draws nothing and changes no height.
constexpr uint32_t TableFullscreen = 1u << 8;
/// A collapsible block's toggle. It shows or hides a body rather than changing
/// a cap, so it joins no chrome bit: `resolveDetails` answers per block, the
/// same way the two caps above do.
constexpr uint32_t DetailsToggle = 1u << 9;
} // namespace ControlBits

/// Every bit the native `controls` prop can carry. `src/controls.ts` sends the
/// same number as its `CONTROL_ALL` default and the codegen prop's default, so
/// the three cannot drift without one of them failing to compile or a test
/// failing.
constexpr uint32_t kControlAll = ControlBits::CodeCopy | ControlBits::CodeDownload | ControlBits::TableCopy |
    ControlBits::TableDownload | ControlBits::CodeWrap | ControlBits::CodeExpand | ControlBits::ImageDownload |
    ControlBits::TableExpand | ControlBits::TableFullscreen | ControlBits::DetailsToggle;

/// True when any code control is on, so a code block gets a header.
constexpr bool hasCodeControls(uint32_t controls) {
  return (controls & (ControlBits::CodeCopy | ControlBits::CodeDownload | ControlBits::CodeWrap |
                      ControlBits::CodeExpand)) != 0;
}

/// True when any table control is on, so a table gets a toolbar.
constexpr bool hasTableControls(uint32_t controls) {
  return (controls & (ControlBits::TableCopy | ControlBits::TableDownload | ControlBits::TableExpand |
                      ControlBits::TableFullscreen)) != 0;
}

/**
 * The chrome a document lays out with, from the `controls` prop and the code
 * layout.
 *
 * A free function rather than a member so the shadow node that measures, the
 * view that lays out, and the worker that prepares cannot disagree about a
 * height. A header and a toolbar are fixed rows, so they are bits. Wrapping is
 * not a bit: a document where the reader has toggled any block has code at two
 * different heights, and no single bit says which. `kChromeCodeWrap` therefore
 * means "every code block in this document wraps", and it is a *cache key*, not
 * the layout itself; `CodeLayout.h`'s `resolveCodeBlock` decides any one block,
 * which is why the whole layout travels beside this rather than inside it.
 *
 * The height cap and the gutter need no bit of their own. A cap changes a
 * block's height only when the block is taller than it, and `resolveCodeBlock`
 * gives that number per block; line numbers narrow the width a wrapped block
 * wraps at, which is a measurement input rather than a chrome row.
 */
inline uint32_t chromeForProps(uint32_t controls, const CodeLayoutProps &code, const ReaderChoices &choices) {
  uint32_t chrome = kChromeNone;
  if (hasCodeControls(controls)) {
    chrome |= kChromeCodeHeader;
  }
  if (hasTableControls(controls)) {
    chrome |= kChromeTableToolbar;
  }
  if ((controls & ControlBits::CodeWrap) != 0 && code.wrap && choices.wrapToggles.empty()) {
    chrome |= kChromeCodeWrap;
  }
  return chrome;
}

} // namespace downstream
