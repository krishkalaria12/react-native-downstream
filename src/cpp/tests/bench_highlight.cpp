// Build with the host engine's sources and -O3 -DNDEBUG. Each result is the
// median of seven samples; token counts keep the workload observable.
#include "CodeHighlighter.h"
#include "IncrementalParser.h"
#include "Export.h"
#include "Languages.h"
#include "StreamLexer.h"
#include "Unicode.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

using namespace downstream;

template <typename Work> void measure(const char *name, Work work, const IncrementalCounters *counters = nullptr) {
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
    std::printf("]");
    if (counters != nullptr) {
      std::printf(",\"bytes_lexed\":%llu,\"runs_built\":%llu,\"token_bytes_copied\":%llu,\"text_bytes_copied\":%llu,\"deferred\":%llu",
                  static_cast<unsigned long long>(counters->highlightBytesLexed),
                  static_cast<unsigned long long>(counters->highlightRunsBuilt),
                  static_cast<unsigned long long>(counters->highlightTokenBytesCopied),
                  static_cast<unsigned long long>(counters->highlightTextBytesCopied),
                  static_cast<unsigned long long>(counters->highlightCount(HighlightStop::Deferred)));
    }
    std::printf("}\n");
  } else {
    std::printf("%s median_ms=%.3f count=%zu", name, median, count);
    if (counters != nullptr) {
      std::printf(" bytes_lexed=%llu runs_built=%llu token_bytes_copied=%llu text_bytes_copied=%llu deferred=%llu",
                  static_cast<unsigned long long>(counters->highlightBytesLexed),
                  static_cast<unsigned long long>(counters->highlightRunsBuilt),
                  static_cast<unsigned long long>(counters->highlightTokenBytesCopied),
                  static_cast<unsigned long long>(counters->highlightTextBytesCopied),
                  static_cast<unsigned long long>(counters->highlightCount(HighlightStop::Deferred)));
    }
    std::printf("\n");
  }
}

void measureLanguageFixtures() {
  const std::pair<const char *, const char *> fixtures[] = {
      {"ts", "export async function load(id: string) { return await fetch(`/api/${id}`); }\n"},
      {"html", "<script>const value = `hello ${name}`;</script><style>.item { color: red; }</style>\n"},
      {"python", "def total(rows):\n    return sum(row.price * row.qty for row in rows) # total\n"},
      {"cpp", "template<class T> T add(T a, T b) { return a + b; } // sum\n"},
      {"rust", "pub fn total(rows: &[u64]) -> u64 { rows.iter().sum() } // sum\n"},
      {"json", "{\"name\": \"downstream\", \"enabled\": true, \"values\": [1, 2, 3]}\n"},
      {"yaml", "services:\n  app:\n    image: downstream:latest\n    ports: [8080, 9090]\n"},
      {"sql", "SELECT name, count(*) FROM users WHERE enabled = true GROUP BY name; -- users\n"},
  };
  for (const auto &[language, sample] : fixtures) {
    const std::string_view line(sample);
    std::string source;
    while (source.size() + line.size() <= 60 * 1024) source += line;
    const std::u16string text = utf8ToUtf16(source);
    const std::string wholeName = std::string("fixture_whole_") + language;
    measure(wholeName.c_str(), [&] {
      HighlightState state;
      std::vector<TokenRun> runs;
      downstreamHighlighter().tokenize(language, text, &state, &runs, 1e9);
      return runs.size();
    });
    const std::string streamName = std::string("fixture_stream_") + language;
    measure(streamName.c_str(), [&] {
      HighlightState state;
      std::vector<TokenRun> runs;
      for (size_t at = 0; at < text.size(); at += 256) {
        downstreamHighlighter().tokenize(language, std::u16string_view(text).substr(0, std::min(at + 256, text.size())),
                                        &state, &runs, 1e9, false);
      }
      downstreamHighlighter().tokenize(language, text, &state, &runs, 1e9);
      return runs.size();
    });
  }
}

int main() {
  measureLanguageFixtures();
  std::string code;
  while (code.size() + 51 <= 60 * 1024) {
    code += "const value = compute(user.id, 42); // next value\n";
  }
  const std::u16string text = utf8ToUtf16(code);
  // What a stream costs the lexer per settled line: restore the saved state,
  // lex one line, save it again.
  measure("lexer_resume_line", [&] {
    StreamLexer *lexer = StreamLexer::current();
    const uint8_t language = findLanguage("ts")->id;
    std::vector<uint8_t> image;
    std::vector<uint32_t> records;
    size_t count = 0;
    bool first = true;
    for (size_t at = 0; at < code.size();) {
      const size_t end = code.find('\n', at) + 1;
      lexer->start(language, first ? nullptr : &image);
      lexer->lex(std::string_view(code).substr(at, end - at), &records);
      lexer->capture(&image);
      count += records.size() / 2;
      first = false;
      at = end;
    }
    return count;
  });
  measure("highlight_whole", [&] {
    size_t count = 0;
    for (int repeat = 0; repeat < 20; repeat++) {
      HighlightState state;
      std::vector<TokenRun> runs;
      downstreamHighlighter().tokenize("ts", text, &state, &runs, 1e9);
      count += runs.size();
    }
    return count;
  });
  measure("highlight_stream_16", [&] {
    HighlightState state;
    std::vector<TokenRun> runs;
    for (size_t at = 16; at < text.size() + 16; at += 16) {
      downstreamHighlighter().tokenize("ts", std::u16string_view(text).substr(0, std::min(at, text.size())),
                                       &state, &runs, 1e9, false);
    }
    downstreamHighlighter().tokenize("ts", text, &state, &runs, 1e9);
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
  std::string longLine;
  while (longLine.size() + 22 <= 60 * 1024) longLine += "compute(user.id, 42) + ";
  for (const bool finish : {false, true}) {
    IncrementalCounters lastCounters;
    measure(finish ? "parser_long_line_finish_16" : "parser_long_line_live_16", [&] {
      IncrementalParser parser(ParseMode::IncrementalTail);
      parser.append("```ts\n");
      parser.parse(false);
      for (size_t at = 0; at < longLine.size(); at += 16) {
        parser.append(std::string_view(longLine).substr(at, 16));
        parser.parse(false);
      }
      const auto result = parser.parse(finish);
      lastCounters = parser.counters();
      return result.document->blocks.back().tokens.size();
    }, &lastCounters);
  }
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
