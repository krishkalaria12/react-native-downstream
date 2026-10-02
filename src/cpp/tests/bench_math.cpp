// Host microbenchmarks; native layout and visible-frame latency are excluded.
#include "downstream/MarkdownParser.h"
#include "downstream/MathScan.h"
#include "math/MathEngine.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#ifdef DOWNSTREAM_BENCH_RATEX
extern "C" downstream::MathEngineResult ratex_parse_and_layout(const char *, const downstream::MathEngineOptions *);
extern "C" void ratex_free_display_list(char *);
extern "C" const char *ratex_get_last_error();
#endif

template <typename Work> void measure(const std::string &name, size_t bytes, int repeats, Work work) {
  std::vector<double> samples;
  uint64_t count = 0;
  work();
  for (int sample = 0; sample < 9; sample++) {
    const auto start = std::chrono::steady_clock::now();
    for (int repeat = 0; repeat < repeats; repeat++) count = work();
    samples.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / repeats);
  }
  auto sorted = samples;
  std::sort(sorted.begin(), sorted.end());
  std::printf("{\"name\":\"%s\",\"bytes\":%zu,\"count\":%llu,\"median_ms\":%.6f,\"samples_ms\":[",
              name.c_str(), bytes, static_cast<unsigned long long>(count), sorted[sorted.size() / 2]);
  for (size_t i = 0; i < samples.size(); i++) std::printf("%s%.6f", i == 0 ? "" : ",", samples[i]);
  std::puts("]}");
}

int main() {
  for (const auto &[name, fragment] : std::vector<std::pair<std::string, std::string>>{
         {"prose", "A paragraph with words, punctuation, and ordinary Markdown.\n\n"},
         {"inline_code", "Use `one` and ``two`` alongside `three` in prose.\n\n"},
         {"fenced_code", "```ts\nconst value = `template`; // next\nconst other = `another`;\n```\n\n"},
         {"bracket_math", "See \\(x^2 + y^2\\) with \\[a+b\\] here.\n\n"},
         {"code_and_math", "Use `code` with \\(x+y\\), then ``more`` with \\[a+b\\].\n\n"}}) {
    std::string source;
    while (source.size() < 100 * 1024) source += fragment;
    measure("scan_" + name, source.size(), 20, [&] {
      return downstream::scan::rewriteMathDelimiters(source, true).pairs.size();
    });
    measure("parse_" + name, source.size(), 10, [&] {
      const auto parsed = downstream::parseMarkdown(source);
      if (parsed.status != downstream::ParseStatus::Ok || !parsed.document) std::abort();
      return parsed.document->blocks.size();
    });
  }
#ifdef DOWNSTREAM_BENCH_RATEX
  downstream::MathEngine::install({ratex_parse_and_layout, ratex_free_display_list, ratex_get_last_error}, "");
  for (const auto &[name, tex] : std::vector<std::pair<std::string, std::string>>{
         {"invalid", "\\frac{a}{"}, {"valid", "a^2+b^2=c^2"}}) {
    measure("ratex_repeated_" + name, tex.size(), 1, [&] {
      downstream::MathEngine::clearCache();
      for (int i = 0; i < 5000; i++) {
        std::string error;
        const auto layout = downstream::MathEngine::layout(tex, true, &error);
        if ((name == "valid") != static_cast<bool>(layout)) std::abort();
      }
      return downstream::MathEngine::engineCallCount();
    });
  }
#endif
}
