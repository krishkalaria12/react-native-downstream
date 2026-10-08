// Release benchmark for the real incremental commit path. Build with the
// engine sources and -O3 -DNDEBUG; compare the same binary fixture across refs.
#include "IncrementalParser.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

using namespace downstream;

int main() {
  for (const size_t count : {256u, 1024u, 4096u}) {
    std::string paragraph;
    for (size_t i = 0; i < count; i++) paragraph += "`[]` **word** ";
    paragraph += "\n\n";
    for (const size_t chunkBytes : {paragraph.size(), size_t{256}}) {
      std::vector<double> samples;
      IncrementalCounters counters;
      for (size_t sample = 0; sample < 10; sample++) {
        IncrementalParser parser(ParseMode::IncrementalTail);
        const auto start = std::chrono::steady_clock::now();
        for (size_t at = 0; at < paragraph.size(); at += chunkBytes) {
          parser.append(std::string_view(paragraph).substr(at, chunkBytes));
          if (parser.parse(false).status != ParseStatus::Ok) return 1;
        }
        const double elapsed = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
        const DocumentHandle document = parser.document();
        if (!document || document->blocks.size() != 1 || document->settledBlocks != 1 ||
            document->blocks.front().identity == 0 || document->blocks.front().runs.size() != count * 2 ||
            parser.committedBytes() != paragraph.size()) {
          std::fputs("unexpected committed bracket paragraph\n", stderr);
          return 1;
        }
        counters = parser.counters();
        // Discard the first iteration before taking the median.
        if (sample > 0) samples.push_back(elapsed);
      }
      std::sort(samples.begin(), samples.end());
      std::printf("inline_code_runs=%zu bytes=%zu chunk=%zu median_ms=%.6f parsed_bytes=%llu commit_attempts=%llu samples_ms=",
                  count, paragraph.size(), chunkBytes, samples[samples.size() / 2],
                  static_cast<unsigned long long>(counters.bytesParsed),
                  static_cast<unsigned long long>(counters.commitAttempts));
      for (size_t i = 0; i < samples.size(); i++) std::printf("%s%.6f", i == 0 ? "" : ",", samples[i]);
      std::putchar('\n');
    }
  }
}
