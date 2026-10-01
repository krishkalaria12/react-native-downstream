#pragma once

#include "Document.h"

#include <algorithm>
#include <span>

namespace downstream {

/// Inline runs are sorted and disjoint. A table cell visits only its
/// overlapping runs, without copying them or rescanning the whole row.
inline std::span<const InlineRun> inlineRunsInRange(const std::vector<InlineRun> &runs,
                                                   uint32_t start, uint32_t end) {
  if (start >= end || runs.empty()) {
    return {};
  }
  const auto first = std::upper_bound(runs.begin(), runs.end(), start,
                                      [](uint32_t offset, const InlineRun &run) { return offset < run.end; });
  const auto last = std::lower_bound(first, runs.end(), end,
                                     [](const InlineRun &run, uint32_t offset) { return run.start < offset; });
  return {first, last};
}

} // namespace downstream
