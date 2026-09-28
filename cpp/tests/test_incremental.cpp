#include "IncrementalParser.h"

#include <array>
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <iterator>
#include <map>
#include <random>
#include <string>
#include <vector>

using namespace downstream;

namespace {

/// Feeds the next canonical source the way a session does: appends the new
/// suffix, which must extend the current source.
ParseResult feed(IncrementalParser &parser, const std::string &source, bool finished) {
  const std::string_view current = parser.source();
  if (source.size() < current.size() || std::string_view(source).substr(0, current.size()) != current) {
    std::cerr << "Test source is not an append\n";
    std::abort();
  }
  parser.append(std::string_view(source).substr(current.size()));
  return parser.parse(finished);
}

ParseResult replaceWith(IncrementalParser &parser, const std::string &source) {
  parser.replace(source);
  return parser.parse(false);
}

bool equalBlock(const Block &x, const Block &y) {
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
  return true;
}

bool equal(const Document &a, const Document &b) {
  if (a.blocks.size() != b.blocks.size() || a.links.size() != b.links.size()) return false;
  for (size_t i = 0; i < a.links.size(); i++) {
    if (a.links[i].url != b.links[i].url || !(a.links[i].range == b.links[i].range)) return false;
  }
  for (size_t i = 0; i < a.blocks.size(); i++) {
    if (!equalBlock(a.blocks[i], b.blocks[i])) return false;
  }
  return true;
}

/// An identity must always name the same block content, source range, and
/// position, since renderers reuse layout by identity alone.
bool stableIdentities(const Document &document, std::map<uint64_t, std::pair<size_t, Block>> &seen) {
  // A committed block stays committed in every later snapshot.
  for (const auto &[identity, entry] : seen) {
    if (entry.first >= document.blocks.size() || document.blocks[entry.first].identity != identity) return false;
  }
  for (size_t i = 0; i < document.blocks.size(); i++) {
    const Block &block = document.blocks[i];
    if (block.identity == 0) continue;
    const auto [entry, added] = seen.try_emplace(block.identity, i, block);
    if (!added && (entry->second.first != i || !equalBlock(entry->second.second, block) ||
                   !(entry->second.second.sourceRange == block.sourceRange))) return false;
  }
  return true;
}

std::vector<std::string> splitChunks(const std::string &text, uint32_t seed) {
  std::mt19937 random(seed);
  std::vector<std::string> chunks;
  size_t start = 0;
  while (start < text.size()) {
    size_t end = std::min(text.size(), start + 1 + random() % 20);
    while (end < text.size() && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) end++;
    chunks.push_back(text.substr(start, end - start));
    start = end;
  }
  return chunks;
}

bool check(const std::vector<std::string> &chunks, const char *label) {
  IncrementalParser incremental(ParseMode::IncrementalTail);
  IncrementalParser full(ParseMode::FullParse);
  std::string source;
  std::map<uint64_t, std::pair<size_t, Block>> identities;
  for (const auto &chunk : chunks) {
    source += chunk;
    const ParseResult actual = feed(incremental, source, false);
    const ParseResult expected = feed(full, source, false);
    if (actual.status != expected.status ||
        (actual.status == ParseStatus::Ok && !equal(*actual.document, *expected.document))) {
      std::cerr << "Mismatch in " << label << " at " << source.size() << " bytes\n";
      return false;
    }
    if (actual.status == ParseStatus::Ok && !stableIdentities(*actual.document, identities)) {
      std::cerr << "Committed identity changed in " << label << "\n";
      return false;
    }
  }
  const auto actual = feed(incremental, source, true);
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
    const auto result = feed(parser, source, false);
    if (result.status != ParseStatus::Ok) return false;
  }
  const auto counts = parser.counters();
  if (counts.tailParses == 0 || counts.cacheHits == 0 ||
      counts.bytesParsed >= source.size() * 100 / 2 || parser.committedBytes() == 0) {
    std::cerr << "Fast path did not save parsed bytes\n";
    return false;
  }
  if (replaceWith(parser, "replacement").status != ParseStatus::Ok ||
      parser.counters().fallbackCount(ParseFallback::NonAppend) != 1) return false;
  source = std::string(70 * 1024, 'x');
  if (replaceWith(parser, source).status != ParseStatus::Ok) return false;
  if (feed(parser, source + "x", false).status != ParseStatus::Ok ||
      parser.counters().fallbackCount(ParseFallback::TailLimit) != 1) return false;
  return true;
}

bool identityRangesAndSnapshots() {
  IncrementalParser parser(ParseMode::IncrementalTail);
  // "café " is 6 UTF-8 bytes but 5 UTF-16 units; the range excludes CRLF.
  const std::string first = "café " + std::string(130, 'x') + "\r\nnext\r\n\r\n";
  const auto before = feed(parser, first, false);
  if (before.status != ParseStatus::Ok || before.document->blocks.size() != 1) return false;
  const Block &committed = before.document->blocks.front();
  if (committed.identity == 0 || committed.sourceRange.start != 0 ||
      committed.sourceRange.end != 141) return false;
  const auto after = feed(parser, first + "[site](https://x)", false);
  if (after.status != ParseStatus::Ok || !equal(*after.document, *parseMarkdown(first + "[site](https://x)").document) ||
      after.document->blocks.front().identity != committed.identity ||
      before.document->blocks.size() != 1 || before.document->links.size() != 0) return false;
  const auto finished = feed(parser, first + "[site](https://x)", true);
  return finished.status == ParseStatus::Ok &&
      finished.document->blocks.front().identity == committed.identity;
}

bool tailLimitPreservesIdentity() {
  IncrementalParser parser(ParseMode::IncrementalTail);
  std::string source = "first paragraph " + std::string(130, 'p') + "\n\n";
  const auto first = feed(parser, source, false);
  if (first.status != ParseStatus::Ok || first.document->blocks.size() != 1) return false;
  const auto identity = first.document->blocks[0].identity;
  const auto range = first.document->blocks[0].sourceRange;
  if (identity == 0) return false;
  source += std::string(70 * 1024, 'x');
  const auto fallback = feed(parser, source, false);
  if (fallback.status != ParseStatus::Ok || fallback.document->blocks[0].identity != identity ||
      parser.counters().fallbackCount(ParseFallback::TailLimit) != 1) return false;
  source += "y";
  const auto repeated = feed(parser, source, false);
  return repeated.status == ParseStatus::Ok && repeated.document->blocks[0].identity == identity &&
      repeated.document->blocks[0].sourceRange == range &&
      parser.counters().fallbackCount(ParseFallback::TailLimit) == 2 &&
      equal(*repeated.document, *parseMarkdown(source).document);
}

bool openFencePath() {
  const std::vector<std::string> chunks = {"```\n", "one", "\n", "two", "\n", "```", "\n\n", "after"};
  if (!check(chunks, "open fence")) return false;
  IncrementalParser parser(ParseMode::IncrementalTail);
  std::string source = "```\n";
  const auto first = feed(parser, source, false);
  if (first.status != ParseStatus::Ok) return false;
  uint64_t lastDocumentId = first.document->id;
  for (int i = 0; i < 100; i++) {
    source += "word\n";
    const auto next = feed(parser, source, false);
    if (next.status != ParseStatus::Ok || next.document->id == lastDocumentId) return false;
    lastDocumentId = next.document->id;
  }
  if (parser.counters().openFenceFastUpdates != 100 ||
      parser.counters().bytesParsed >= source.size() * 10) return false;
  source += "```\n";
  const auto closing = feed(parser, source, false);
  return closing.status == ParseStatus::Ok &&
      equal(*closing.document, *parseMarkdown(source).document);
}

// A list item line at a byte offset of 4 modulo 256 makes md4c end the
// list after two blank lines, so partial parses must keep full-parse offsets.
// Committed prose of every length puts the list at every offset residue, in
// the tail parse and in the full parse.
bool md4cOffsetQuirk() {
  for (size_t extra = 0; extra < 256; extra++) {
    const std::string prose = "Committed prose, long enough to pass the minimum commit size before the list "
                              "starts streaming" + std::string(extra, '.') + " and keeps going.\n\n";
    const std::vector<std::string> chunks = {prose, "  - nested\n", "\n", "\n", "    indented code\n", "\nafter\n"};
    if (!check(chunks, "md4c offset quirk")) return false;
  }
  return true;
}

// The open-fence fast path copies bytes verbatim, so it must refuse bytes
// md4c rewrites and must not mistake a closing fence for a new opener.
bool fenceEdgeCases() {
  const std::vector<std::vector<std::string>> cases = {
      {"```\n", "a\n", "\tb\n", "c"},
      {"```py\n", "x = 1\r\n", "y"},
      {"# title\n```\n", "```\n", "**"},
      {"~~~\n", "a\n", "```\n", "b\n", "~~~\n", "after"},
      {"````\n", "```\n", "inner\n", "````\n", "done"},
      {"```\n", "a\n", "  ``", "`\n", "b"},
  };
  for (const auto &chunks : cases) {
    if (!check(chunks, "fence edge case")) return false;
  }
  return true;
}

// Reaching the committed block limit must stop committing, not force two
// parses of the whole source on every update.
bool blockLimitKeepsTail() {
  IncrementalParser parser(ParseMode::IncrementalTail);
  std::string source;
  for (int i = 0; i < 8300; i++) {
    source += "w\n\n";
    if (feed(parser, source, false).status != ParseStatus::Ok) return false;
  }
  const auto before = parser.counters();
  const auto first = parser.document()->blocks.front().identity;
  for (int i = 0; i < 10; i++) {
    source += "w\n\n";
    if (feed(parser, source, false).status != ParseStatus::Ok) return false;
  }
  const auto after = parser.counters();
  return first != 0 && parser.document()->blocks.front().identity == first &&
      after.fullParses == before.fullParses && after.bytesParsed - before.bytesParsed < source.size() &&
      equal(*parser.document(), *parseMarkdown(source).document);
}

// Successive snapshots share committed blocks instead of copying them.
bool snapshotsShareCommittedBlocks() {
  IncrementalParser parser(ParseMode::IncrementalTail);
  const std::string first = std::string(200, 'a') + "\n\n# Heading\n\nactive";
  const auto before = feed(parser, first, false);
  const auto after = feed(parser, first + " tail", false);
  return before.status == ParseStatus::Ok && after.status == ParseStatus::Ok &&
      before.document->blocks.sharedSize() == 2 && after.document->blocks.sharedSize() == 2 &&
      &before.document->blocks[0] == &after.document->blocks[0] &&
      &before.document->blocks[1] == &after.document->blocks[1] && after.document->blocks.tail().size() == 1;
}

bool identitiesAreGlobal() {
  const std::string source = std::string(200, 'a') + "\n\nnext";
  IncrementalParser first(ParseMode::IncrementalTail);
  IncrementalParser second(ParseMode::IncrementalTail);
  const auto a = feed(first, source, false);
  const auto b = feed(second, source, false);
  second.reset();
  const auto c = feed(second, source, false);
  return a.status == ParseStatus::Ok && b.status == ParseStatus::Ok && c.status == ParseStatus::Ok &&
      a.document->blocks[0].identity != 0 && a.document->blocks[0].identity != b.document->blocks[0].identity &&
      b.document->blocks[0].identity != c.document->blocks[0].identity;
}

// A typical assistant answer commits prose, headings, lists, tables, and
// closed fences, and streams open fences without reparsing.
bool realisticAnswer() {
  const std::string answer = R"md(Sure! Here's a practical walkthrough of a **streaming Markdown renderer**. I'll cover the architecture, the key APIs, and a few pitfalls.

## Why stream at all?

When a model generates text token by token, re-rendering the *whole* document on every token is wasteful: a 4,000-word answer can mean thousands of full parses. See the [CommonMark spec](https://spec.commonmark.org/0.31.2/) for why block boundaries are subtle.

1. Re-parse everything on each update (simple, but O(n^2) overall).
2. Parse only the "active tail" and keep completed blocks.

## Setting up the session

```tsx
export function Answer({ chunks }: { chunks: AsyncIterable<string> }) {
  const [stream] = useState(() => createMarkdownStream());
  return <DownstreamMarkdown stream={stream} selectable />;
}
```

> **Note:** `finish()` re-renders the raw source once more, so preview repair disappears.

| Chunk size | Updates/sec |
|-----------:|------------:|
| 1 char     | 60          |
| 8 chars    | 60          |

- **Setext headings.** A line of `===` after a paragraph turns it into a heading.
- **Lists.** A blank line inside a list doesn't end it.

That's it -- you now have a streaming renderer that stays responsive even for long answers.

)md";
  std::string text;
  for (int i = 0; i < 4; i++) text += answer;
  const auto chunks = splitChunks(text, 7);
  if (!check(chunks, "realistic answer")) return false;
  IncrementalParser incremental(ParseMode::IncrementalTail);
  IncrementalParser full(ParseMode::FullParse);
  std::string source;
  for (const auto &chunk : chunks) {
    source += chunk;
    feed(incremental, source, false);
    feed(full, source, false);
  }
  const auto counts = incremental.counters();
  if (counts.tailParses == 0 || counts.openFenceFastUpdates == 0 || counts.committedBlocks == 0 ||
      counts.bytesParsed * 5 > full.counters().bytesParsed) {
    std::cerr << "Realistic answer parsed " << counts.bytesParsed << " of " << full.counters().bytesParsed
              << " full-parse bytes\n";
    return false;
  }
  return true;
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
      identityRangesAndSnapshots() && tailLimitPreservesIdentity() && openFencePath() &&
      md4cOffsetQuirk() && fenceEdgeCases() && blockLimitKeepsTail() && snapshotsShareCommittedBlocks() && identitiesAreGlobal() &&
      realisticAnswer() ? 0 : 1;
}
