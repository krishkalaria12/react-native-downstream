// Release host timings for PR #22. Native layout and paint are excluded.
#include "downstream/MarkdownParser.h"
#include "downstream/Scan.h"
#include "remend/Preview.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace downstream;

uint64_t fingerprint(const ParseResult &parsed) {
  if (parsed.status != ParseStatus::Ok || !parsed.document) std::abort();
  uint64_t hash = 14695981039346656037ull;
  const auto add = [&](uint64_t value) { hash = (hash ^ value) * 1099511628211ull; };
  for (const auto &block : parsed.document->blocks) {
    add(static_cast<uint8_t>(block.kind));
    for (const char16_t unit : block.text) add(unit);
    for (const auto &run : block.runs) {
      add(run.start); add(run.end); add(run.styles); add(run.link);
    }
    for (const auto &cell : block.cells) { add(cell.start); add(cell.end); }
  }
  return hash;
}

template <typename Work> void measure(const std::string &name, size_t bytes, int repeats, Work work) {
  const uint64_t expected = work();
  std::vector<double> samples;
  for (int sample = 0; sample < 9; sample++) {
    const auto start = std::chrono::steady_clock::now();
    for (int repeat = 0; repeat < repeats; repeat++) {
      if (work() != expected) std::abort();
    }
    samples.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / repeats);
  }
  auto sorted = samples;
  std::sort(sorted.begin(), sorted.end());
  std::printf("{\"name\":\"%s\",\"bytes\":%zu,\"fingerprint\":\"%llu\",\"median_ms\":%.6f,\"samples_ms\":[",
              name.c_str(), bytes, static_cast<unsigned long long>(expected), sorted[sorted.size() / 2]);
  for (size_t i = 0; i < samples.size(); i++) std::printf("%s%.6f", i == 0 ? "" : ",", samples[i]);
  std::puts("]}");
}

int main() {
  const ParseOptions options{ParseFlags::InlineHtml};
  const remend::Preview preview{remend::PreviewOptions::all()};
  for (const auto &[name, fragment] : std::vector<std::pair<std::string, std::string>>{
         {"prose", "Words in a plain paragraph with punctuation. "},
         {"scripts", "H<sub>2</sub>O and x<sup>2</sup> next. "},
         {"styled_breaks", "H<sub>2</sub>O **bold** <br> x<sup>2</sup> "},
         {"code_markers", "Use `a*b_c~d` and **bold** with `*x_y~z` here. "}}) {
    for (const size_t kib : {10, 50, 100}) {
      std::string source;
      while (source.size() < kib * 1024) source += fragment;
      measure("parse_" + name + "_" + std::to_string(kib) + "k", source.size(), 5,
              [&] { return fingerprint(parseMarkdown(source, options)); });
      measure("preview_" + name + "_" + std::to_string(kib) + "k", source.size(), 3,
              [&] { return fingerprint(preview.apply(source, 0, 0, options).parsed); });
    }
    if (name == "scripts" || name == "code_markers") {
      std::string source;
      while (source.size() < 10 * 1024) source += fragment;
      measure("preview_stream_" + name + "_64b", source.size(), 1, [&] {
        uint64_t hash = 0;
        for (size_t end = 64; end < source.size(); end += 64) {
          hash = (hash * 1099511628211ull) ^ fingerprint(preview.apply(std::string_view(source).substr(0, end), 0, 0, options).parsed);
        }
        return hash ^ fingerprint(preview.apply(source, 0, 0, options).parsed);
      });
    }
  }
  std::string source;
  while (source.size() < 10 * 1024) source += "H<sub>2</sub>O and x<sup>2</sup> next. ";
  source += "<su";
  measure("preview_partial_tag_10k", source.size(), 5,
          [&] { return fingerprint(preview.apply(source, 0, 0, options).parsed); });
  const std::string longTag = "<" + std::string(64 * 1024, 'a') + ">";
  measure("reject_long_tag", longTag.size(), 200,
          [&] { return static_cast<uint64_t>(scan::matchInlineTag(longTag)); });
}
