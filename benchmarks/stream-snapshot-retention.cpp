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

bool matchesPrefix(const DocumentHandle &document, std::string_view code, size_t length) {
  if (!document || document->blocks.size() != 1 || document->blocks.back().text.size() != length) return false;
  const auto &text = document->blocks.back().text;
  for (size_t index = 0; index < length; index += 97) {
    if (text[index] != static_cast<char16_t>(code[index])) return false;
  }
  return length == 0 || text[length - 1] == static_cast<char16_t>(code[length - 1]);
}

} // namespace

int main(int argc, char **argv) {
  const std::string mode = argc > 1 ? argv[1] : "retained";
  if (mode != "retained" && mode != "latest") return 1;
  constexpr size_t kBytes = 60 * 1024;
  constexpr size_t kChunk = 16;
  const std::string unit = "const value = compute(user.id, 42); ";
  std::string code;
  while (code.size() < kBytes) code += unit;
  code.resize(kBytes);
  const std::string opener = "```\n";
  std::vector<DocumentHandle> retained;
  if (mode == "retained") retained.reserve(kBytes / kChunk);
  const uint64_t peakBefore = peakResidentBytes();
  const auto started = std::chrono::steady_clock::now();
  IncrementalParser parser(ParseMode::IncrementalTail);
  parser.append(opener);
  if (parser.parse(false).status != ParseStatus::Ok) return 2;
  for (size_t start = 0; start < code.size(); start += kChunk) {
    parser.append(std::string_view(code).substr(start, kChunk));
    const auto result = parser.parse(false);
    if (result.status != ParseStatus::Ok) return 2;
    if (mode == "retained") retained.push_back(result.document);
  }
  const double elapsedMs = std::chrono::duration<double, std::milli>(
      std::chrono::steady_clock::now() - started).count();
  // Capture before any contiguous text consumer or final highlighting pass.
  // This is whole-process high-water RSS, not an isolated allocation count.
  const uint64_t peakAfter = peakResidentBytes();
  const DocumentHandle latest = parser.document();
  if (peakAfter == 0 || parser.source() != opener + code || !matchesPrefix(latest, code, code.size())) return 3;
  uint64_t checksum = latest->blocks.back().text.size();
  for (size_t i = 0; i < retained.size(); i++) {
    const size_t length = (i + 1) * kChunk;
    if (!matchesPrefix(retained[i], code, length)) return 3;
    checksum += retained[i]->blocks.back().text[length - 1];
  }
  parser.append("more body text");
  if (parser.parse(false).status != ParseStatus::Ok || !matchesPrefix(latest, code, code.size())) return 4;
  for (size_t i = 0; i < retained.size(); i++) {
    if (!matchesPrefix(retained[i], code, (i + 1) * kChunk)) return 4;
  }
  parser.append("\n```\n");
  const auto finished = parser.parse(true);
  if (finished.status != ParseStatus::Ok || finished.document->blocks.size() != 1 ||
      finished.document->blocks.back().text != utf8ToUtf16(code + "more body text")) return 5;
  std::printf("{\"mode\":\"%s\",\"bytes\":%zu,\"chunk\":%zu,\"retained_snapshots\":%zu,"
              "\"peak_before_bytes\":%llu,\"peak_after_bytes\":%llu,\"stream_ms\":%.6f,\"checksum\":%llu}\n",
      mode.c_str(), kBytes, kChunk, retained.size(),
      static_cast<unsigned long long>(peakBefore), static_cast<unsigned long long>(peakAfter),
      elapsedMs, static_cast<unsigned long long>(checksum));
}
