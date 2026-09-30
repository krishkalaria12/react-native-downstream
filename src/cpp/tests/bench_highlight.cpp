// Build with the host engine's sources and -O3 -DNDEBUG. Each result is the
// median of seven samples; token counts keep the workload observable.
#include "CodeDocument.h"
#include "CodeHighlighter.h"
#include "IncrementalParser.h"
#include "Export.h"
#include "Unicode.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
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
  auto ordered = samples;
  std::sort(ordered.begin(), ordered.end());
  const double median = ordered[ordered.size() / 2];
  if (std::getenv("DOWNSTREAM_BENCH_JSON") != nullptr) {
    std::printf("{\"name\":\"%s\",\"median_ms\":%.6f,\"count\":%zu,\"samples_ms\":[", name, median, count);
    for (size_t index = 0; index < samples.size(); index++) {
      std::printf("%s%.6f", index == 0 ? "" : ",", samples[index]);
    }
    std::printf("]}\n");
  } else {
    std::printf("%s median_ms=%.3f count=%zu\n", name, median, count);
  }
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
  Document wide;
  Block row;
  row.kind = BlockKind::TableRow;
  for (uint32_t column = 0; column < 2048; column++) {
    const uint32_t start = static_cast<uint32_t>(row.text.size());
    row.text.append(u"plain styled tail");
    row.cells.push_back({start, static_cast<uint32_t>(row.text.size())});
    row.runs.push_back({start + 6, start + 12, InlineStyle::Strong});
    row.text.push_back(u'\t');
  }
  wide.blocks.push_back(std::move(row));
  for (const ExportFormat format : {ExportFormat::Markdown, ExportFormat::Html}) {
    measure(format == ExportFormat::Markdown ? "export_wide_markdown" : "export_wide_html", [&] {
      size_t count = 0;
      for (int repeat = 0; repeat < 20; repeat++) {
        count += exportBlocks(wide, 0, 1, format).size();
      }
      return count;
    });
  }
  for (const std::string_view workload : {"mixed", "fences", "references"}) {
    const bool fences = workload != "mixed";
    const bool references = workload == "references";
    std::string source = references ? "[ref]\n\n" : "";
    while (source.size() < 16 * 1024) {
      source += fences ? "```ts\nconst value = compute(user.id, 42);\nconst other = compute(value, 1);\n```\n\n"
                       : "## Heading\n\nA paragraph with **strong** and [link](https://example.com).\n\n```ts\nconst value = compute(user.id, 42);\n```\n\n";
    }
    for (const size_t chunk : {size_t(32), size_t(128)}) {
      for (const ParseMode mode : {ParseMode::FullParse, ParseMode::IncrementalTail}) {
        const std::string name = std::string(workload) + "_" +
            (mode == ParseMode::FullParse ? "full_" : "incremental_") + std::to_string(chunk);
        measure(name.c_str(), [&] {
          IncrementalParser parser(mode);
          if (references) {
            parser.append(std::string(200, 'p') + "\n\n");
            parser.parse(false);
          }
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
