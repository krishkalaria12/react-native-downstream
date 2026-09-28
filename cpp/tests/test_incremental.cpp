#include "IncrementalParser.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

using namespace downstream;

namespace {

bool equal(const Document &a, const Document &b) {
  if (a.blocks.size() != b.blocks.size() || a.links.size() != b.links.size()) return false;
  for (size_t i = 0; i < a.links.size(); i++) {
    if (a.links[i].url != b.links[i].url || !(a.links[i].range == b.links[i].range)) return false;
  }
  for (size_t i = 0; i < a.blocks.size(); i++) {
    const auto &x = a.blocks[i], &y = b.blocks[i];
    if (x.kind != y.kind || x.headingLevel != y.headingLevel || x.tableHeader != y.tableHeader ||
        x.table != y.table || x.spacingBefore != y.spacingBefore || x.text != y.text ||
        x.language != y.language || x.runs.size() != y.runs.size() ||
        x.cells.size() != y.cells.size() || x.containers.size() != y.containers.size()) return false;
    for (size_t j = 0; j < x.runs.size(); j++) {
      const auto &p = x.runs[j], &q = y.runs[j];
      if (p.start != q.start || p.end != q.end || p.styles != q.styles || p.link != q.link) return false;
    }
    for (size_t j = 0; j < x.cells.size(); j++) {
      const auto &p = x.cells[j], &q = y.cells[j];
      if (p.start != q.start || p.end != q.end || p.align != q.align) return false;
    }
    for (size_t j = 0; j < x.containers.size(); j++) {
      const auto &p = x.containers[j], &q = y.containers[j];
      if (p.kind != q.kind || p.id != q.id || p.showsMarker != q.showsMarker ||
          p.task != q.task || p.marker != q.marker) return false;
    }
  }
  return true;
}

bool check(const std::vector<std::string> &chunks, const char *label) {
  IncrementalParser incremental(ParseMode::IncrementalTail);
  IncrementalParser full(ParseMode::FullParse);
  std::string source;
  uint64_t firstIdentity = 0;
  for (const auto &chunk : chunks) {
    source += chunk;
    const ParseResult actual = incremental.update(source, true, false);
    const ParseResult expected = full.update(source, true, false);
    if (actual.status != expected.status ||
        (actual.status == ParseStatus::Ok && !equal(*actual.document, *expected.document))) {
      std::cerr << "Mismatch in " << label << " at " << source.size() << " bytes\n";
      return false;
    }
    if (actual.status == ParseStatus::Ok && !actual.document->blocks.empty() &&
        actual.document->blocks.front().identity != 0) {
      if (firstIdentity != 0 && firstIdentity != actual.document->blocks.front().identity) {
        std::cerr << "Committed identity changed in " << label << "\n";
        return false;
      }
      firstIdentity = actual.document->blocks.front().identity;
    }
  }
  const auto actual = incremental.update(source, true, true);
  const auto expected = parseMarkdown(source);
  if (actual.status != expected.status ||
      (actual.status == ParseStatus::Ok && !equal(*actual.document, *expected.document))) {
    std::cerr << "Finish mismatch in " << label << "\n";
    return false;
  }
  return true;
}

bool seededCases() {
  constexpr std::array<const char *, 27> tokens = {
      "plain", " text", "\n", "\n\n", "\r\n", "# title\n", "---\n", "| a | b |\n",
      "| - | - |\n", "- item\n", "> quote\n", "```js\n", "```\n", "[link]",
      "(https://x)", "[ref]", "\n[ref]: https://x\n", "**", "_", "!", "`",
      "👩‍💻", "漢字", "  ", "<div>\n", "</div>\n", "a\r\nb\r\n"};
  std::mt19937 random(0x5eed1234);
  for (int sample = 0; sample < 160; sample++) {
    std::vector<std::string> chunks;
    for (int i = 0; i < 24; i++) chunks.emplace_back(tokens[random() % tokens.size()]);
    if (!check(chunks, "seeded")) return false;
  }
  return true;
}

bool fastPathAndFallbacks() {
  IncrementalParser parser(ParseMode::IncrementalTail);
  std::string source;
  for (int i = 0; i < 100; i++) {
    source += "plain paragraph words\n\n";
    const auto result = parser.update(source, true, false);
    if (result.status != ParseStatus::Ok) return false;
  }
  const auto counts = parser.counters();
  if (counts.tailParses == 0 || counts.cacheHits == 0 ||
      counts.bytesParsed >= source.size() * 100 / 2 || parser.committedBytes() == 0) {
    std::cerr << "Fast path did not save parsed bytes\n";
    return false;
  }
  if (parser.update("replacement", false, false).status != ParseStatus::Ok ||
      parser.counters().fallbackCount(ParseFallback::NonAppend) != 1) return false;
  source = std::string(70 * 1024, 'x');
  if (parser.update(source, false, false).status != ParseStatus::Ok) return false;
  if (parser.update(source + "x", true, false).status != ParseStatus::Ok ||
      parser.counters().fallbackCount(ParseFallback::TailLimit) != 1) return false;
  return true;
}

bool identityRangesAndSnapshots() {
  IncrementalParser parser(ParseMode::IncrementalTail);
  const std::string first = "café\r\nnext\r\n\r\n";
  const auto before = parser.update(first, true, false);
  if (before.status != ParseStatus::Ok || before.document->blocks.size() != 1) return false;
  const Block &committed = before.document->blocks.front();
  if (committed.identity == 0 || committed.sourceRange.start != 0 ||
      committed.sourceRange.end != 10) return false;
  const auto after = parser.update(first + "[site](https://x)", true, false);
  if (after.status != ParseStatus::Ok || !equal(*after.document, *parseMarkdown(first + "[site](https://x)").document) ||
      after.document->blocks.front().identity != committed.identity ||
      before.document->blocks.size() != 1 || before.document->links.size() != 0) return false;
  const auto finished = parser.update(first + "[site](https://x)", true, true);
  return finished.status == ParseStatus::Ok &&
      finished.document->blocks.front().identity == committed.identity;
}

bool openFencePath() {
  const std::vector<std::string> chunks = {"```\n", "one", "\n", "two", "\n", "```", "\n\n", "after"};
  if (!check(chunks, "open fence")) return false;
  IncrementalParser parser(ParseMode::IncrementalTail);
  std::string source = "```\n";
  const auto first = parser.update(source, true, false);
  if (first.status != ParseStatus::Ok) return false;
  uint64_t lastDocumentId = first.document->id;
  for (int i = 0; i < 100; i++) {
    source += "word\n";
    const auto next = parser.update(source, true, false);
    if (next.status != ParseStatus::Ok || next.document->id == lastDocumentId) return false;
    lastDocumentId = next.document->id;
  }
  if (parser.counters().openFenceFastUpdates != 100 ||
      parser.counters().bytesParsed >= source.size() * 10) return false;
  source += "```\n";
  const auto closing = parser.update(source, true, false);
  return closing.status == ParseStatus::Ok &&
      equal(*closing.document, *parseMarkdown(source).document);
}

} // namespace

int main() {
  std::vector<std::string> chunks;
  while (true) {
    uint32_t length = 0;
    if (!std::cin.read(reinterpret_cast<char *>(&length), sizeof(length))) break;
    std::string chunk(length, '\0');
    if (!std::cin.read(chunk.data(), length)) return 2;
    chunks.push_back(std::move(chunk));
  }
  return check(chunks, "fixture") && seededCases() && fastPathAndFallbacks() &&
      identityRangesAndSnapshots() && openFencePath() ? 0 : 1;
}
