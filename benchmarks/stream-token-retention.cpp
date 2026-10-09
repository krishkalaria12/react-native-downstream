#include "CodeHighlighter.h"
#include "IncrementalParser.h"
#include "Unicode.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <string_view>
#include <sys/resource.h>
#include <vector>

using namespace downstream;

namespace {

uint64_t peakResidentBytes() {
  rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage) != 0) return 0;
#if defined(__APPLE__)
  return static_cast<uint64_t>(usage.ru_maxrss);
#else
  return static_cast<uint64_t>(usage.ru_maxrss) * 1024;
#endif
}

uint64_t tokenChecksum(const Block &block) {
  uint64_t checksum = 1469598103934665603ULL;
  for (const auto &token : block.tokens) {
    checksum = (checksum ^ token.start) * 1099511628211ULL;
    checksum = (checksum ^ token.end) * 1099511628211ULL;
    checksum = (checksum ^ static_cast<uint64_t>(token.token)) * 1099511628211ULL;
  }
  return checksum;
}

bool validSnapshot(const DocumentHandle &document, size_t length) {
  if (!document || document->blocks.size() != 1) return false;
  const auto &block = document->blocks.back();
  if (block.text.size() != length || !block.highlightComplete || block.tokens.empty()) return false;
  return block.tokens[0].start == 0 && block.tokens[0].end == 3 && block.tokens.back().end == length;
}

bool matchesCheckedTokens(const Block &block) {
  HighlightState state;
  std::vector<TokenRun> expected;
  lexillaHighlighter().tokenize("ts", block.text, &state, &expected, 1e9);
  if (block.tokens.size() != expected.size()) return false;
  for (size_t index = 0; index < expected.size(); index++) {
    const auto &actual = block.tokens[index];
    const auto &wanted = expected[index];
    if (actual.start != wanted.start || actual.end != wanted.end || actual.token != wanted.token) return false;
  }
  return true;
}

} // namespace

int main(int argc, char **argv) {
  const std::string mode = argc > 1 ? argv[1] : "retained";
  if (mode != "retained" && mode != "latest") return 1;
  constexpr size_t kUpdates = 5200;
  const std::string unit = "let x=1;\n";
  const std::string opener = "```ts\n";
  std::string code;
  for (size_t index = 0; index < kUpdates; index++) code += unit;
  std::vector<DocumentHandle> retained;
  if (mode == "retained") retained.reserve(kUpdates);
  const uint64_t peakBefore = peakResidentBytes();
  const auto started = std::chrono::steady_clock::now();
  IncrementalParser parser(ParseMode::IncrementalTail);
  parser.append(opener);
  if (parser.parse(false).status != ParseStatus::Ok) return 2;
  for (size_t index = 0; index < kUpdates; index++) {
    parser.append(unit);
    const auto result = parser.parse(false);
    if (result.status != ParseStatus::Ok) return 2;
    if (mode == "retained") retained.push_back(result.document);
  }
  const double elapsedMs = std::chrono::duration<double, std::milli>(
      std::chrono::steady_clock::now() - started).count();
  // Sample before checksum/validation requests contiguous text or finishes.
  const uint64_t peakAfter = peakResidentBytes();
  const auto counters = parser.counters();
  const DocumentHandle latest = parser.document();
  if (peakAfter == 0 || parser.source() != opener + code ||
      !validSnapshot(latest, code.size() - 1) || !matchesCheckedTokens(latest->blocks.back())) return 3;
  uint64_t checksum = tokenChecksum(latest->blocks.back());
  for (size_t index = 0; index < retained.size(); index++) {
    const size_t length = (index + 1) * unit.size() - 1;
    if (!validSnapshot(retained[index], length)) return 3;
    checksum = (checksum ^ retained[index]->blocks.back().tokens.size()) * 1099511628211ULL;
    if (index % 97 == 0) {
      if (!matchesCheckedTokens(retained[index]->blocks.back())) return 3;
      checksum = (checksum ^ tokenChecksum(retained[index]->blocks.back())) * 1099511628211ULL;
    }
  }
  parser.append("let after=2;\n");
  if (parser.parse(false).status != ParseStatus::Ok || !validSnapshot(latest, code.size() - 1) ||
      !matchesCheckedTokens(latest->blocks.back())) return 4;
  for (size_t index = 0; index < retained.size(); index += 97) {
    if (!matchesCheckedTokens(retained[index]->blocks.back())) return 4;
  }
  parser.append("```\n");
  const auto finished = parser.parse(true);
  if (finished.status != ParseStatus::Ok || finished.document->blocks.size() != 1 ||
      finished.document->blocks.back().text != utf8ToUtf16(code + "let after=2;") ||
      !matchesCheckedTokens(finished.document->blocks.back())) return 5;
  std::printf("{\"mode\":\"%s\",\"bytes\":%zu,\"chunk\":%zu,\"updates\":%zu,\"retained_snapshots\":%zu,"
              "\"final_tokens\":%zu,\"peak_before_bytes\":%llu,\"peak_after_bytes\":%llu,\"stream_ms\":%.6f,"
              "\"token_payload_bytes_copied\":%llu,\"highlight_text_bytes_copied\":%llu,\"highlight_bytes_lexed\":%llu,"
              "\"checksum\":\"%llu\"}\n",
      mode.c_str(), code.size(), unit.size(), kUpdates, retained.size(), latest->blocks.back().tokens.size(),
      static_cast<unsigned long long>(peakBefore), static_cast<unsigned long long>(peakAfter), elapsedMs,
      static_cast<unsigned long long>(counters.highlightTokenBytesCopied),
      static_cast<unsigned long long>(counters.highlightTextBytesCopied),
      static_cast<unsigned long long>(counters.highlightBytesLexed),
      static_cast<unsigned long long>(checksum));
}
