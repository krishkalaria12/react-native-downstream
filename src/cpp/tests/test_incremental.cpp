#include "IncrementalParser.h"
#include "SegmentPlan.h"
#include "check.h"

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
    std::cerr << "FAIL " << __FILE__ << ": test source is not an append\n";
    return ParseResult{};
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

/// Full mode carries identities for unchanged leading blocks. A block that
/// later changes loses its identity, but an identity still names one content,
/// source range, and position, and identified blocks form a prefix that ends
/// after a top-level block outside a continuing table.
bool carriedIdentities(const Document &document, std::map<uint64_t, std::pair<size_t, Block>> &seen) {
  size_t prefix = 0;
  while (prefix < document.blocks.size() && document.blocks[prefix].identity != 0) prefix++;
  for (size_t i = prefix; i < document.blocks.size(); i++) {
    if (document.blocks[i].identity != 0) return false;
  }
  if (prefix > 0) {
    const Block &last = document.blocks[prefix - 1];
    if (!last.containers.empty() || prefix == document.blocks.size()) return false;
    if (last.kind == BlockKind::TableRow && document.blocks[prefix].kind == BlockKind::TableRow &&
        document.blocks[prefix].table == last.table) return false;
  }
  for (size_t i = 0; i < prefix; i++) {
    const Block &block = document.blocks[i];
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
  std::map<uint64_t, std::pair<size_t, Block>> fullIdentities;
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
    if (expected.status == ParseStatus::Ok) {
      const auto oracle = parseMarkdown(source);
      if (!equal(*expected.document, *oracle.document) ||
          !carriedIdentities(*expected.document, fullIdentities)) {
        std::cerr << "Full-mode identity broke in " << label << " at " << source.size() << " bytes\n";
        return false;
      }
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

// Renderers reuse committed segments by first identity and block count, so a
// committed segment that is followed by another must keep its boundaries in
// every later snapshot, and a text chunk must not split a quote or list.
bool committedSegmentsAreStable(ParseMode mode) {
  std::string text;
  for (int i = 0; i < 40; i++) {
    text += "Paragraph " + std::to_string(i) + " has enough words to be worth committing on its own line.\n\n";
    if (i % 7 == 3) text += "> quoted\n> lines\n\n- one\n- two\n\n| a | b |\n|---|---|\n| 1 | 2 |\n\n";
  }
  IncrementalParser parser(mode);
  std::map<std::pair<uint64_t, size_t>, size_t> sealed;
  std::string source;
  size_t committedSegments = 0;
  for (const auto &chunk : splitChunks(text, 3)) {
    source += chunk;
    const auto result = feed(parser, source, false);
    if (result.status != ParseStatus::Ok) return false;
    const Document &document = *result.document;
    const auto plan = planSegments(document);
    for (size_t i = 0; i < plan.size(); i++) {
      const SegmentPlan &segment = plan[i];
      if (!segment.committed) continue;
      committedSegments++;
      // Text chunks also end where a table starts; any other end is top-level.
      const bool beforeTable = segment.to < document.blocks.size() &&
          document.blocks[segment.to].kind == BlockKind::TableRow;
      if (!segment.table && !beforeTable && !document.blocks[segment.to - 1].containers.empty()) return false;
      for (size_t block = segment.from; block < segment.to; block++) {
        if (document.blocks[block].identity == 0) return false;
      }
      // A committed segment followed by another committed segment is sealed.
      if (i + 1 < plan.size() && plan[i + 1].committed) {
        const auto key = std::make_pair(document.blocks[segment.from].identity, segment.to - segment.from);
        const auto [entry, added] = sealed.try_emplace(key, segment.from);
        if (!added && entry->second != segment.from) return false;
      }
    }
    // Every sealed chunk seen so far is still present with the same extent.
    for (const auto &[key, from] : sealed) {
      if (from >= document.blocks.size() || document.blocks[from].identity != key.first) return false;
      bool found = false;
      for (const SegmentPlan &segment : plan) {
        found = found || (segment.from == from && segment.to - segment.from == key.second);
      }
      if (!found) {
        std::cerr << "Sealed committed segment changed\n";
        return false;
      }
    }
  }
  return committedSegments > 0 && sealed.size() > 2;
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

// Full mode reparses everything, but a platform should lay out only the
// changed tail: nearly every finished block keeps the identity it first got.
bool fullModeKeepsIdentities() {
  std::string text;
  for (int i = 0; i < 30; i++) {
    text += "## Section " + std::to_string(i) + "\n\nSome prose with **bold** and a [link](https://x.dev/" +
        std::to_string(i) + ").\n\n- one\n- two\n\n```js\nconst a = " + std::to_string(i) + ";\n```\n\n";
  }
  IncrementalParser parser(ParseMode::FullParse);
  std::string source;
  size_t assigned = 0;
  std::map<uint64_t, bool> identities;
  DocumentHandle last;
  for (const auto &chunk : splitChunks(text, 11)) {
    source += chunk;
    const auto result = feed(parser, source, false);
    if (result.status != ParseStatus::Ok) return false;
    for (const Block &block : result.document->blocks) {
      if (block.identity != 0 && identities.emplace(block.identity, true).second) assigned++;
    }
    last = result.document;
  }
  const auto finished = feed(parser, source, true);
  if (finished.status != ParseStatus::Ok) return false;
  const size_t blocks = finished.document->blocks.size();
  size_t kept = 0;
  for (size_t i = 0; i < blocks; i++) {
    kept += finished.document->blocks[i].identity != 0 &&
        finished.document->blocks[i].identity == last->blocks[i].identity;
  }
  // Each block is identified about once, and the finish parse keeps the
  // streamed identities. Only the trailing list and the last block, which
  // no top-level block has closed yet, stay unidentified.
  if (assigned > blocks + blocks / 10 || kept + 3 < blocks) {
    std::cerr << "Full mode assigned " << assigned << " identities to " << blocks << " blocks and kept " << kept
              << " at finish\n";
    return false;
  }
  return true;
}

/* A frame length of kEndOfDocument closes the current document. Lengths are
   otherwise byte counts, so no chunk can collide with the sentinel. */
static constexpr uint32_t kEndOfDocument = 0xffffffffu;

/* Reads one length-prefixed document. Returns false at end of input. A short
   read means the stream was truncated, which the caller reports. */
static bool readDocument(std::vector<std::string> &chunks, bool *truncated) {
  *truncated = false;
  chunks.clear();
  while (true) {
    uint32_t length = 0;
    if (!std::cin.read(reinterpret_cast<char *>(&length), sizeof(length))) return !chunks.empty();
    if (length == kEndOfDocument) return true;
    std::string chunk(length, '\0');
    if (length > 0 && !std::cin.read(chunk.data(), length)) {
      *truncated = true;
      return true;
    }
    chunks.push_back(std::move(chunk));
  }
}

/* Sweeps many documents in one process. The corpus in tests/ is far larger
   than the per-run budget of a single document, and spawning a process per
   case would dominate the suite. Every document is checked, so one run reports
   every regression rather than only the first. */
static int batchedCorpus() {
  std::vector<std::string> chunks;
  bool truncated = false;
  size_t documents = 0;
  while (readDocument(chunks, &truncated)) {
    if (truncated) {
      std::cerr << "Truncated frame in batched corpus\n";
      return 2;
    }
    const size_t index = documents++;
    // check() reports the offset where a document diverged, so the index
    // identifies which corpus entry needs fixing.
    char label[64];
    snprintf(label, sizeof(label), "corpus[%zu]", index);
    CHECK(check(chunks, label), "batched corpus streams match a full parse");
    if (ds_check_failures > 0) return ds_check_result();
  }
  return ds_check_result();
}

} // namespace

int main(int argc, char **argv) {
  if (argc > 1 && std::string(argv[1]) == "--batch") return batchedCorpus();
  std::vector<std::string> chunks;
  bool truncated = false;
  readDocument(chunks, &truncated);
  if (truncated) return 2;
  // Every case runs even when an earlier one fails, so one run reports every
  // regression rather than only the first.
  CHECK(check(chunks, "fixture"), "fixture streams match a full parse");
  CHECK(seededCases(), "seeded cases");
  CHECK(fastPathAndFallbacks(), "fast path and fallbacks");
  CHECK(identityRangesAndSnapshots(), "identity ranges and snapshots");
  CHECK(tailLimitPreservesIdentity(), "tail limit preserves identity");
  CHECK(openFencePath(), "open fence path");
  CHECK(md4cOffsetQuirk(), "md4c offset quirk");
  CHECK(fenceEdgeCases(), "fence edge cases");
  CHECK(blockLimitKeepsTail(), "block limit keeps the tail");
  CHECK(snapshotsShareCommittedBlocks(), "snapshots share committed blocks");
  CHECK(committedSegmentsAreStable(ParseMode::IncrementalTail),
        "committed segments are stable (incremental)");
  CHECK(committedSegmentsAreStable(ParseMode::FullParse),
        "committed segments are stable (full)");
  CHECK(identitiesAreGlobal(), "identities are global");
  CHECK(realisticAnswer(), "realistic answer");
  CHECK(fullModeKeepsIdentities(), "full mode keeps identities");
  return ds_check_result();
}
