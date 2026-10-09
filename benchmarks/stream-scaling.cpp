#include "Compare.h"
#include "IncrementalParser.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

using namespace downstream;

// Only explicitly incomplete highlighting may differ in a live snapshot.
// Source, parsed markup, metadata and final tokens must still match exactly.
bool sameLiveDocument(const Document &actual, const Document &expected) {
  if (actual.blocks.size() != expected.blocks.size()) return false;
  Document left(actual), right(expected);
  for (size_t index = 0; index < actual.blocks.size(); ++index) {
    const auto &block = actual.blocks[index];
    if (block.kind != BlockKind::CodeBlock || block.highlightComplete) continue;
    for (const auto &token : block.tokens) {
      if (token.start > token.end || token.end > block.text.size()) return false;
    }
    if (index < left.blocks.sharedSize() || index < right.blocks.sharedSize()) return false;
    left.blocks.tail()[index - left.blocks.sharedSize()].tokens.clear();
    right.blocks.tail()[index - right.blocks.sharedSize()].tokens.clear();
  }
  return scan::sameDocument(left, right);
}

int main(int argc, char **argv) {
  const bool check = argc > 1 && std::string(argv[1]) == "--check";
  for (const std::string mode : {"plain", "definition", "unbroken", "ascii-tail", "fence-lines", "fence-single", "fence-plain-single"}) {
    for (size_t bytes : {size_t{16384}, size_t{32768}, size_t{61440}, size_t{98304}}) {
      const bool fence = mode.find("fence") == 0;
      if (fence && bytes > 61440) continue;
      std::string source;
      if (fence) source = mode == "fence-plain-single" ? "```\n" : "```ts\n";
      else if (mode == "definition") source = "[ref]: https://example.test\n\n";
      else if (mode == "plain") source = "plain\n\n";
      const size_t opener = source.size();
      const std::string unit = mode == "unbroken" ? "Normal paragraph text with strong wording. " :
          mode == "ascii-tail" ? "Normal paragraph text with strong wording " :
          (mode == "fence-single" || mode == "fence-plain-single") ? "const value = compute(user.id, 42); " :
          mode == "fence-lines" ? "const value = compute(user.id, 42); // next value\n" :
          "Normal paragraph text with **strong** wording.\n\n";
      while (source.size() < bytes + (fence ? opener : 0)) source += unit;
      source.resize(bytes + (fence ? opener : 0));
      const size_t chunk = fence ? 16 : 128;
      std::vector<double> samples;
      IncrementalCounters counters;
      size_t committed = 0;
      bool complete = false;
      uint64_t flattenedTailBytes = 0;
      for (int sample = 0; sample < (check ? 1 : 6); ++sample) {
        const auto started = std::chrono::steady_clock::now();
        IncrementalParser parser(ParseMode::IncrementalTail);
        size_t at = 0;
        if (fence) {
          parser.append(std::string_view(source).substr(0, opener));
          if (parser.parse(false).status != ParseStatus::Ok) return 2;
          at = opener;
        }
        for (; at < source.size(); at += chunk) {
          parser.append(std::string_view(source).substr(at, chunk));
          if (parser.parse(false).status != ParseStatus::Ok) return 2;
        }
        const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        counters = parser.counters();
        committed = parser.committedBytes();
        complete = parser.document()->blocks.empty() || parser.document()->blocks.back().highlightComplete;
#if __has_include("SnapshotText.h")
        if (!parser.document()->blocks.empty()) flattenedTailBytes = parser.document()->blocks.back().text.flattenedBytes();
#endif
        if (check) {
          IncrementalParser full(ParseMode::FullParse);
          full.append(source);
          const auto open = full.parse(false);
          if (parser.source() != source || open.status != ParseStatus::Ok ||
              !sameLiveDocument(*parser.document(), *open.document)) {
            std::fprintf(stderr, "Live parity failure: %s %zu\n", mode.c_str(), bytes);
            return 3;
          }
          const auto finished = parser.parse(true);
          const auto expected = full.parse(true);
          if (finished.status != ParseStatus::Ok || expected.status != ParseStatus::Ok ||
              !scan::sameDocument(*finished.document, *expected.document)) {
            std::fprintf(stderr, "Finished parity failure: %s %zu\n", mode.c_str(), bytes);
            return 4;
          }
        } else if (sample != 0) samples.push_back(elapsed);
      }
      auto sorted = samples;
      std::sort(sorted.begin(), sorted.end());
      std::printf("{\"mode\":\"%s\",\"bytes\":%zu,\"chunk\":%zu,\"median_ms\":%.6f,\"parsed_bytes\":%llu,\"full\":%llu,\"tail\":%llu,\"fast\":%llu,\"committed\":%zu,\"highlight_complete\":%s,",
          mode.c_str(), bytes, chunk, sorted.empty() ? 0 : sorted[sorted.size() / 2],
          static_cast<unsigned long long>(counters.bytesParsed), static_cast<unsigned long long>(counters.fullParses),
          static_cast<unsigned long long>(counters.tailParses), static_cast<unsigned long long>(counters.openFenceFastUpdates),
          committed, complete ? "true" : "false");
#if __has_include("SnapshotText.h")
      std::printf("\"fence_snapshot_storage_bytes_copied\":%llu,\"fence_append_scan_bytes\":%llu,\"flattened_last_tail_bytes\":%llu,",
          static_cast<unsigned long long>(counters.fenceSnapshotStorageBytesCopied),
          static_cast<unsigned long long>(counters.fenceAppendScanBytes),
          static_cast<unsigned long long>(flattenedTailBytes));
#else
      (void)flattenedTailBytes;
      std::printf("\"fence_snapshot_storage_bytes_copied\":null,\"fence_append_scan_bytes\":null,\"flattened_last_tail_bytes\":null,");
#endif
      std::printf("\"samples_ms\":[");
      for (size_t index = 0; index < samples.size(); ++index) std::printf("%s%.6f", index ? "," : "", samples[index]);
      std::puts("]}");
    }
  }
}
