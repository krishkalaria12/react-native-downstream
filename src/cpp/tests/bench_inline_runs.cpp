// Range-selection benchmark only. No text shaping, UIKit or drawing is timed.
#include "downstream/InlineRuns.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace downstream;

namespace {

volatile uint64_t consumed = 0;

uint64_t fullScan(const std::vector<InlineRun> &runs, uint32_t start, uint32_t end) {
  uint64_t sum = 0;
  for (const InlineRun &run : runs) {
    const uint32_t from = std::max(start, run.start);
    const uint32_t to = std::min(end, run.end);
    if (from < to) sum += to - from + run.styles;
  }
  return sum;
}

uint64_t rangeScan(const std::vector<InlineRun> &runs, uint32_t start, uint32_t end) {
  uint64_t sum = 0;
  for (const InlineRun &run : inlineRunsInRange(runs, start, end)) {
    sum += std::min(end, run.end) - std::max(start, run.start) + run.styles;
  }
  return sum;
}

template <typename Scan>
double measure(const std::vector<InlineRun> &runs, size_t columns, size_t passes, Scan scan) {
  const auto begin = std::chrono::steady_clock::now();
  uint64_t sum = 0;
  for (size_t pass = 0; pass < passes; ++pass) {
    for (uint32_t column = 0; column < columns; ++column) {
      sum += scan(runs, column * 16, column * 16 + 16);
    }
  }
  consumed = sum;
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
}

void workload(size_t columns, size_t passes) {
  std::vector<InlineRun> runs;
  for (uint32_t column = 0; column < columns; ++column) {
    runs.push_back({column * 16 + 1, column * 16 + 5, InlineStyle::Strong, -1});
    runs.push_back({column * 16 + 8, column * 16 + 14, InlineStyle::Emphasis, -1});
  }
  for (uint32_t column = 0; column < columns; ++column) {
    if (fullScan(runs, column * 16, column * 16 + 16) != rangeScan(runs, column * 16, column * 16 + 16)) {
      std::abort();
    }
  }
  measure(runs, columns, 2, fullScan);
  measure(runs, columns, 2, rangeScan);
  std::printf("{\"columns\":%zu,\"runs\":%zu,\"passes\":%zu,\"beforeMs\":[", columns, runs.size(), passes);
  std::vector<double> before;
  std::vector<double> after;
  // Alternate order to avoid giving one implementation every warm run.
  for (size_t sample = 0; sample < 7; ++sample) {
    if (sample % 2 == 0) {
      before.push_back(measure(runs, columns, passes, fullScan));
      after.push_back(measure(runs, columns, passes, rangeScan));
    } else {
      after.push_back(measure(runs, columns, passes, rangeScan));
      before.push_back(measure(runs, columns, passes, fullScan));
    }
  }
  for (size_t sample = 0; sample < before.size(); ++sample) std::printf("%s%.6f", sample == 0 ? "" : ",", before[sample]);
  std::printf("],\"afterMs\":[");
  for (size_t sample = 0; sample < after.size(); ++sample) std::printf("%s%.6f", sample == 0 ? "" : ",", after[sample]);
  std::printf("]}");
}

} // namespace

int main() {
  std::printf("[\n");
  workload(128, 100);
  std::printf(",\n");
  workload(2048, 20);
  std::printf("\n]\n");
}
