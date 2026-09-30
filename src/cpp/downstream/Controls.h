#pragma once

#include "RenderPreparer.h"

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

/**
 * The chrome a document lays out with, from the `controls` prop, the
 * `codeBlockWrap` prop, and the reader's own wrap choices.
 *
 * A free function rather than a member so the shadow node that measures, the
 * view that lays out, and the worker that prepares cannot disagree about a
 * height. A header and a toolbar are fixed rows, so they are bits. Wrapping is
 * not: a document where the reader has toggled any block has code at two
 * different heights, so the wrap bit becomes "no code in this document scrolls
 * sideways", and a document with both wrapped and unwrapped code is measured
 * again whenever the set changes.
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
