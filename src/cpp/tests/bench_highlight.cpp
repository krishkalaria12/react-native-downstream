// Build with the host engine's sources and -O3 -DNDEBUG. Each result is the
// median of seven samples; token counts keep the workload observable.
#include "CodeDocument.h"
#include "CodeHighlighter.h"
#include "IncrementalParser.h"
#include "Unicode.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

using namespace downstream;

template <typename Work> void measure(const char *name, Work work) {
  std::vector<double> samples;
  size_t count = 0;
  work();
  for (int sample = 0; sample < 7; sample++) {
    const auto started = std::chrono::steady_clock::now();
    count = work();
    samples.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
  }
  std::sort(samples.begin(), samples.end());
  std::printf("%s median_ms=%.3f count=%zu\n", name, samples[samples.size() / 2], count);
}

int main() {
  std::string code;
  while (code.size() + 51 <= 60 * 1024) {
    code += "const value = compute(user.id, 42); // next value\n";
  }
  const std::u16string text = utf8ToUtf16(code);
  measure("document_append_16", [&] {
    size_t count = 0;
    for (int repeat = 0; repeat < 20; repeat++) {
      CodeDocument document;
      document.reset("");
      for (size_t at = 0; at < code.size(); at += 16) {
        document.append(std::string_view(code).substr(at, 16));
      }
      count += document.lineCount();
    }
    return count;
  });
  measure("highlight_whole", [&] {
    size_t count = 0;
    for (int repeat = 0; repeat < 20; repeat++) {
      HighlightState state;
      std::vector<TokenRun> runs;
      lexillaHighlighter().tokenize("ts", text, &state, &runs, 1e9);
      count += runs.size();
    }
    return count;
  });
  measure("highlight_stream_16", [&] {
    HighlightState state;
    std::vector<TokenRun> runs;
    for (size_t at = 16; at < text.size() + 16; at += 16) {
      lexillaHighlighter().tokenize("ts", std::u16string_view(text).substr(0, std::min(at, text.size())),
                                   &state, &runs, 1e9);
    }
    return runs.size();
  });
  measure("parser_stream_16", [&] {
    IncrementalParser parser(ParseMode::IncrementalTail);
    parser.append("```ts\n");
    parser.parse(false);
    for (size_t at = 0; at < code.size(); at += 16) {
      parser.append(std::string_view(code).substr(at, 16));
      parser.parse(false);
    }
    return parser.parse(false).document->blocks.back().tokens.size();
  });
  for (const bool fences : {false, true}) {
    std::string source;
    while (source.size() < 16 * 1024) {
      source += fences ? "```ts\nconst value = compute(user.id, 42);\nconst other = compute(value, 1);\n```\n\n"
                       : "## Heading\n\nA paragraph with **strong** and [link](https://example.com).\n\n```ts\nconst value = compute(user.id, 42);\n```\n\n";
    }
    for (const size_t chunk : {size_t(32), size_t(128)}) {
      for (const ParseMode mode : {ParseMode::FullParse, ParseMode::IncrementalTail}) {
        const std::string name = std::string(fences ? "fences" : "mixed") + "_" +
            (mode == ParseMode::FullParse ? "full_" : "incremental_") + std::to_string(chunk);
        measure(name.c_str(), [&] {
          IncrementalParser parser(mode);
          for (size_t at = 0; at < source.size(); at += chunk) {
            parser.append(std::string_view(source).substr(at, chunk));
            parser.parse(false);
          }
          return parser.parse(true).document->blocks.size();
        });
      }
    }
  }
}
