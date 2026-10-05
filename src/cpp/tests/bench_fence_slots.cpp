// Host fence-slot costs. The harness disables the unrelated prepared-document
// cache identically for both candidates; no layout, highlighting or paint runs.
#include "downstream/ControlledSlots.h"
#include "downstream/PreparedDocument.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace downstream;

namespace downstream {
std::shared_ptr<const PreparedDocument> PreparedDocument::completed(std::string_view, ParseOptions) {
  return nullptr;
}
} // namespace downstream

namespace {
volatile uint64_t sink = 0;
bool validateOnly = false;

uint64_t fingerprint(const ControlledSlots &answer) {
  uint64_t hash = 14695981039346656037ull;
  const auto add = [&](uint64_t value) { hash = (hash ^ value) * 1099511628211ull; };
  add(answer.pending);
  add(answer.slots.size());
  for (const auto &slot : answer.slots) {
    add(slot.ordinal); add(slot.blockIndex); add(slot.incomplete); add(slot.identity);
    for (const auto &text : {slot.language, slot.meta}) {
      add(text.size());
      for (unsigned char unit : text) add(unit);
    }
    add(slot.code.size());
    for (char16_t unit : slot.code) add(unit);
  }
  return hash;
}

template <typename Work>
void measure(const std::string &name, size_t bytes, int repeats, Work work) {
  const auto expected = work();
  const uint64_t hash = fingerprint(expected);
  // Warm code and allocators, including the cache-hit workload's cache.
  for (int i = 0; i < (validateOnly ? 1 : 8); i++) {
    if (fingerprint(work()) != hash) std::abort();
  }
  std::vector<double> samples;
  if (!validateOnly) {
    for (int sample = 0; sample < 9; sample++) {
      const auto start = std::chrono::steady_clock::now();
      for (int repeat = 0; repeat < repeats; repeat++) {
        const auto answer = work();
        sink = sink ^ answer.slots.size() ^ static_cast<uint64_t>(answer.pending);
      }
      samples.push_back(std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - start).count());
      // Full validation is outside the measured interval.
      if (fingerprint(work()) != hash) std::abort();
    }
  }
  auto sorted = samples;
  std::sort(sorted.begin(), sorted.end());
  std::printf("{\"name\":\"%s\",\"bytes\":%zu,\"slots\":%zu,\"pending\":%s,\"fingerprint\":\"%llu\",\"iterations\":%d,\"median_total_ms\":%.6f,\"samples_total_ms\":[",
      name.c_str(), bytes, expected.slots.size(), expected.pending ? "true" : "false",
      static_cast<unsigned long long>(hash), validateOnly ? 0 : repeats,
      sorted.empty() ? 0 : sorted[sorted.size() / 2]);
  for (size_t i = 0; i < samples.size(); i++) std::printf("%s%.6f", i ? "," : "", samples[i]);
  std::puts("]}");
}

void run(const std::string &name, const std::string &source) {
  const ParseOptions parse;
  const std::vector<std::string> languages{"chart", "mermaid"};
  measure("controlled_cold_" + name, source.size(), 100, [&] {
    clearControlledSlotCache();
    return controlledFenceSlots(source, parse, languages);
  });
  clearControlledSlotCache();
  measure("controlled_repeat_" + name, source.size(), 1000, [&] {
    return controlledFenceSlots(source, parse, languages);
  });
  measure("parsed_" + name, source.size(), 100, [&] {
    return ControlledSlots{false, parsedFenceSlots(source, parse, languages)};
  });
#if defined(DOWNSTREAM_BENCH_RESOLVED_SLOTS)
  const auto resolved = [&] { return ControlledSlots{false, resolvedFenceSlots(source, parse, languages)}; };
#else
  // PR head used parsedFenceSlots directly on its async worker. This shim
  // measures that same old worker call with the identical harness.
  const auto resolved = [&] { return ControlledSlots{false, parsedFenceSlots(source, parse, languages)}; };
#endif
  measure("worker_cold_" + name, source.size(), 100, [&] {
    clearControlledSlotCache();
    return resolved();
  });
  clearControlledSlotCache();
  measure("worker_repeat_" + name, source.size(), 1000, resolved);
}
} // namespace

int main(int argc, char **argv) {
  validateOnly = argc > 1 && std::string(argv[1]) == "--check";
  for (size_t kib : {10, 50, 100}) {
    std::string prose = "```chart title=Revenue\n[1,2,3]\n```\n\n";
    while (prose.size() < kib * 1024) prose += "A **styled** paragraph with a [link](https://example.com) and `code`.\n\n";
    run("prose_" + std::to_string(kib) + "k", prose);
    std::string fence = "```chart title=Revenue\n";
    while (fence.size() < kib * 1024) fence += "[1,2,3,4,5,6,7,8,9],\n";
    run("open_code_" + std::to_string(kib) + "k", fence);
  }
  for (size_t count : {64, 256}) {
    std::string source;
    for (size_t index = 0; index < count; index++) {
      source += "```chart title=Series\n[" + std::to_string(index) + ",2,3]\n```\n\n";
      source += "```json\n{\"ordinary\":true}\n```\n\n";
    }
    run("many_fences_" + std::to_string(count), source);
  }
}
