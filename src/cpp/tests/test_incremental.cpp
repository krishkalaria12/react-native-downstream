#include "CodeBlocks.h"
#include "CodeHighlighter.h"
#include "Compare.h"
#include "IncrementalParser.h"
#include "SegmentPlan.h"
#include "check.h"
#include "tests/dump_model.h"

#include <algorithm>
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
      x.cells.size() != y.cells.size() || x.containers.size() != y.containers.size() ||
      x.tokens.size() != y.tokens.size()) return false;
  for (size_t j = 0; j < x.runs.size(); j++) {
    const auto &p = x.runs[j], &q = y.runs[j];
    if (p.start != q.start || p.end != q.end || p.styles != q.styles || p.link != q.link) return false;
  }
  // Tokens are part of what a reader sees, so a block whose colors changed is a
  // changed block: a streamed document has to reach the same tokens as a full
  // parse of the same bytes.
  for (size_t j = 0; j < x.tokens.size(); j++) {
    const auto &p = x.tokens[j], &q = y.tokens[j];
    if (p.start != q.start || p.end != q.end || p.token != q.token) return false;
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

/// A raw parse plus the highlighting a session does, so the oracle is what a
/// reader would actually see rather than an uncolored intermediate.
DocumentHandle colored(ParseResult result, double budgetMs = kHighlightBudgetMs) {
  // Full-mode and controlled documents settle their code before publishing.
  // A single budgeted pass is an oracle for an unfinished streaming tail,
  // whose tokens may still be partial under CPU load.
  return highlightSettledCodeBlocks(lexillaHighlighter(), result.document, budgetMs, nullptr);
}

bool fullParseOracleSettles() {
  std::string source = "```ts\n";
  for (int line = 0; line < 80; line++) {
    source += "const value = compute(42);\n";
  }
  source += "```\n\nSettled prose.\n";
  const ParseResult parsed = parseMarkdown(source);
  IncrementalParser parser(ParseMode::FullParse);
  parser.append(source);
  const ParseResult actual = parser.parse(false);
  // A zero budget guarantees a stop after the first window, even on a fast
  // host. A raw full-parse oracle must settle its tokens just as full mode does.
  const DocumentHandle partial = highlightCodeBlocks(lexillaHighlighter(), parsed.document, 0.0, nullptr);
  return actual.status == ParseStatus::Ok && !equal(*partial, *actual.document) &&
      equal(*colored(parsed, 0.0), *actual.document);
}

/// `parse` is the parse options every parse in this case runs under, so a
/// corpus can be swept with inline HTML or an opt-in syntax on as well as off.
bool check(const std::vector<std::string> &chunks, const char *label, ParseOptions parse = ParseOptions()) {
  IncrementalParser incremental(ParseMode::IncrementalTail, parse);
  IncrementalParser full(ParseMode::FullParse, parse);
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
      // The oracle is a raw parse, so it is colored the way the full-mode
      // parser colors before it compares anything.
      const auto oracle = colored(parseMarkdown(source, parse));
      if (!equal(*expected.document, *oracle) ||
          !carriedIdentities(*expected.document, fullIdentities)) {
        std::cerr << "Full-mode identity broke in " << label << " at " << source.size() << " bytes\n";
        return false;
      }
    }
  }
  const auto actual = feed(incremental, source, true);
  const auto expected = colored(parseMarkdown(source, parse));
  if (actual.status != ParseStatus::Ok || !equal(*actual.document, *expected)) {
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

// Highlighting appended fence text must leave every retained snapshot intact,
// including a token completed on the current line and a pending final newline.
bool fenceAppendTokensAndSnapshots() {
  const std::string code = "const value = compute(user.id, 42); // wide \xF0\x9F\x91\x8D\n"
                           "const next = \"\xC3\xA9\xE6\xBC\xA2\";\n\nreturn value;\n";
  for (const char *language : {"ts", "py", "rs", "json", "html", "unknown"}) {
    for (size_t chunk : {size_t(1), size_t(7), size_t(16)}) {
      IncrementalParser parser(ParseMode::IncrementalTail);
      std::string source = std::string("```") + language + " title=sample\n";
      CHECK(feed(parser, source, false).status == ParseStatus::Ok, "the test fence opens");
      std::vector<DocumentHandle> retained;
      std::vector<Block> copies;
      for (size_t at = 0; at < code.size();) {
        size_t take = std::min(chunk, code.size() - at);
        // Session chunks are valid UTF-8 strings; keep whole scalars even for
        // the one-byte workload. The standalone highlighter tests separately
        // cover UTF-16 surrogate pairs arriving in two calls.
        while (at + take < code.size() && (static_cast<unsigned char>(code[at + take]) & 0xc0) == 0x80) take++;
        source.append(code, at, take);
        at += take;
        const auto result = feed(parser, source, false);
        CHECK(result.status == ParseStatus::Ok, "the streaming fence parses");
        const auto parsedFull = parseMarkdown(source);
        const auto full = highlightSettledCodeBlocks(lexillaHighlighter(), parsedFull.document, 1e9, nullptr);
        CHECK(full != nullptr, "the full-parse oracle accepts the UTF-8 fence");
        if (!full || !result.document) return false;
        CHECK(equal(*result.document, *full), "every growing fence matches fresh text and tokens");
        retained.push_back(result.document);
        copies.push_back(result.document->blocks.back());
      }
      CHECK(parser.counters().openFenceFastUpdates > 1, "the exact-lineage path receives multiple append updates");
      source += "```\n\n```py\nreturn 1";
      CHECK(feed(parser, source, false).status == ParseStatus::Ok, "closing and changing language reparses");
      source += "2\nreturn 3";
      const auto after = feed(parser, source, false);
      const auto full = highlightSettledCodeBlocks(lexillaHighlighter(), parseMarkdown(source).document, 1e9, nullptr);
      CHECK(equal(*after.document, *full), "a different fence cannot inherit an earlier fence's lineage");
      for (size_t i = 0; i < retained.size(); i++) {
        CHECK(equalBlock(retained[i]->blocks.back(), copies[i]), "later appends cannot mutate retained text or tokens");
        CHECK(retained[i]->blocks.back().info == copies[i].info, "retained fence metadata is unchanged");
      }
      parser.replace("```ts\nconst changed = 'reset';");
      CHECK(parser.parse(false).status == ParseStatus::Ok, "reset invalidates the old fence");
      parser.append("\nconst more = 1");
      CHECK(parser.parse(false).status == ParseStatus::Ok, "a reset fence resumes safely");
      parser.append("2;");
      const auto reset = parser.parse(false);
      const auto resetFull = highlightSettledCodeBlocks(lexillaHighlighter(), parseMarkdown(parser.source()).document, 1e9, nullptr);
      CHECK(equal(*reset.document, *resetFull), "reset append tokens match a fresh pass");
    }
  }
  return true;
}

// The open-fence fast path copies bytes verbatim, so it must refuse bytes
// md4c rewrites and must not mistake a closing fence for a new opener.
bool fenceEdgeCases() {
  CHECK(fenceAppendTokensAndSnapshots(), "fence append tokens and retained snapshots");
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
      // Text chunks also end where a table or a code block starts; any other
      // end is top-level.
      const bool beforeTable = segment.to < document.blocks.size() &&
          document.blocks[segment.to].kind == BlockKind::TableRow;
      if (!segment.isTable() && !beforeTable && !document.blocks[segment.to - 1].containers.empty()) return false;
      // A code block is a segment of its own, so nothing else shares its range.
      if (segment.isCode() &&
          (segment.to - segment.from != 1 || document.blocks[segment.from].kind != BlockKind::CodeBlock)) {
        return false;
      }
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
bool unchangedTailCodeIsCached() {
  for (const ParseMode mode : {ParseMode::FullParse, ParseMode::IncrementalTail}) {
    IncrementalParser parser(mode);
    std::string source = std::string(200, 'p') + "\n\n";
    if (feed(parser, source, false).status != ParseStatus::Ok) return false;
    // An unresolved reference prevents this region from committing, so its
    // closed code fence stays in the active tail while the prose grows.
    source += "[ref]\n\n```ts\nconst value = compute(42);\n```\n\nprose";
    const auto first = feed(parser, source, false);
    if (first.status != ParseStatus::Ok || !first.document->blocks[2].highlightComplete) return false;
    const auto before = parser.counters();
    for (int update = 0; update < 30; update++) {
      source += " more prose";
      if (feed(parser, source, false).status != ParseStatus::Ok) return false;
    }
    const auto after = parser.counters();
    if (before.blocksHighlighted == 0 || after.blocksHighlighted != before.blocksHighlighted ||
        (mode == ParseMode::IncrementalTail && after.tailParses <= before.tailParses)) return false;
    source += "\n\n[ref]: https://example.com\n";
    const auto finished = feed(parser, source, true);
    if (finished.status != ParseStatus::Ok || !equal(*finished.document, *colored(parseMarkdown(source)))) return false;
  }
  return true;
}

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
static int batchedCorpus(ParseOptions parse) {
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
    CHECK(check(chunks, label, parse), "batched corpus streams match a full parse");
    if (ds_check_failures > 0) return ds_check_result();
  }
  return ds_check_result();
}

bool inlineCodeBracketsAndReferences() {
  std::string code;
  for (size_t i = 0; i < 96; i++) {
    code += "**`[ref]`** plain *`[]`* ~~`[other]`~~ [inline](/inline) ";
  }
  // Exercise an unresolved bracket before, between, and after code runs,
  // including a bracket in a non-code run with the same enclosing style.
  const std::vector<std::string> paragraphs = {
      code,
      "[ref] " + code,
      code.substr(0, code.size() / 2) + " [ref] " + code.substr(code.size() / 2),
      code + " [ref]",
      code + " **[ref]**",
      code + " *[ref]*",
  };
  for (size_t caseIndex = 0; caseIndex < paragraphs.size(); caseIndex++) {
    IncrementalParser parser(ParseMode::IncrementalTail);
    std::string source = paragraphs[caseIndex] + "\n\n";
    const auto first = feed(parser, source, false);
    if (!scan::sameDocument(*first.document, *parseMarkdown(source).document)) return false;
    const auto held = first.document;
    const Block heldBlock = held->blocks.front();
    if (caseIndex == 0) {
      if (heldBlock.identity == 0 || held->settledBlocks != 1 || parser.committedBytes() != source.size()) {
        return false;
      }
    } else if (heldBlock.identity != 0 || held->settledBlocks != 0 || parser.committedBytes() != 0) {
      return false;
    }
    bool combinedCodeStyle = false;
    for (const Block &block : first.document->blocks) {
      uint32_t previousEnd = 0;
      for (const InlineRun &run : block.runs) {
        if (run.start < previousEnd || run.end <= run.start || run.end > block.text.size()) return false;
        previousEnd = run.end;
        combinedCodeStyle = combinedCodeStyle ||
            ((run.styles & InlineStyle::Code) != 0 && run.styles != InlineStyle::Code);
      }
    }
    if (!combinedCodeStyle) return false;

    std::map<uint64_t, std::pair<size_t, Block>> identities;
    if (!stableIdentities(*first.document, identities)) return false;
    // Stream the definition byte by byte so a split "]:" must still revoke
    // provisional text, while code brackets keep their literal contents.
    const std::string suffix = "Next paragraph with `[]`.\n\n[ref]: /resolved\n";
    for (const char byte : suffix) {
      source.push_back(byte);
      const auto next = feed(parser, source, false);
      if (!scan::sameDocument(*next.document, *parseMarkdown(source).document) ||
          !stableIdentities(*next.document, identities) || !equalBlock(held->blocks.front(), heldBlock)) {
        return false;
      }
    }
    const auto finished = parser.parse(true);
    if (!scan::sameDocument(*finished.document, *parseMarkdown(source).document) ||
        finished.document->settledBlocks != finished.document->blocks.size() ||
        !equalBlock(held->blocks.front(), heldBlock)) return false;
  }
  return true;
}

// A definition-free reference-bearing prefix is reusable, but stays mutable:
// no provisional block gets an identity or becomes available as settled text.
bool referenceSafeReuse() {
  const std::string padding = "\n\n" + std::string(160, 'p') + "\n\n";
  const std::vector<std::string> cases = {
      "[ref]" + padding + "[ref]: https://example.com\n",
      "[label][ref] [ref][] ![alt][ref] ![ref]" + padding + "[ref]: /image.png \"title\"\n",
      "[Ref] [REF][] [label][rEf]" + padding + "[ref]: /first\n[REF]: /second\n",
      "[two words]" + padding + "[two\n words]:\n /target\n \"multiline title\"\n",
      "[ref]: /before\n\n[ref]" + padding + "[ref]: /duplicate\n",
      "\\[ref] [missing] `[:code]` [inline](/target)" + padding + "[ref]: /escaped\n[missing]: /resolved\n",
      "[^note]" + padding + "[^note]: footnote **content**\n    next line\n",
      "> [ref]\n> quoted\n\n- [ref]\n- ![ref]" + padding + "> [ref]: /inside\n",
      "| [ref] | b |\n| --- | --- |\n| ![ref] | c |" + padding + "[ref]: /table\n",
      "[ref]" + padding + "```\n[ref]: /code\n```\n\n[ref]: /outside\n",
      "[ref]" + padding + "\\[\n" + std::string(160, 'm') + "\n\n\\]\n",
      "[ref]" + padding + "\\(" + std::string(160, 'm') + "\\)\n",
      "[ref]" + padding + "<div>\n[ref]: /html\n</div>\n\n[ref]: /outside\n",
      "![ref]" + padding + "[ref]: <https://example.com/image.png> 'title'\n",
      "&#91;ref&#93; [ref]" + padding + "[ref]: /entity\n",
      "[ref]" + padding + "\\[ref]: /escaped-marker\n",
  };
  for (const auto &text : cases) {
    IncrementalParser parser(ParseMode::IncrementalTail);
    std::map<uint64_t, std::pair<size_t, Block>> identities;
    std::vector<std::pair<DocumentHandle, Document>> snapshots;
    std::string source;
    // Every byte exercises split brackets, colons, titles, and definitions.
    for (const char byte : text) {
      source.push_back(byte);
      const auto result = feed(parser, source, false);
      const auto expected = colored(parseMarkdown(source));
      if (result.status != ParseStatus::Ok || !scan::sameDocument(*result.document, *expected) ||
          !stableIdentities(*result.document, identities)) {
        std::cerr << "Reference prefix mismatch at " << source.size() << " bytes in " << text << "\n";
        return false;
      }
      if (source.size() % 79 == 0) {
        Document copy = *result.document;
        copy.blocks = SharedList<Block>(nullptr, std::vector<Block>(copy.blocks.begin(), copy.blocks.end()));
        copy.links = SharedList<Link>(nullptr, std::vector<Link>(copy.links.begin(), copy.links.end()));
        copy.images = SharedList<Image>(nullptr, std::vector<Image>(copy.images.begin(), copy.images.end()));
        snapshots.emplace_back(result.document, std::move(copy));
      }
    }
    for (const auto &[handle, copy] : snapshots) {
      if (!scan::sameDocument(*handle, copy) || handle->settledBlocks != copy.settledBlocks) return false;
    }
    const auto finished = parser.parse(true);
    if (!scan::sameDocument(*finished.document, *colored(parseMarkdown(source))) ||
        finished.document->settledBlocks != finished.document->blocks.size()) return false;
  }

  IncrementalParser parser(ParseMode::IncrementalTail);
  std::string source = std::string(160, 's') + "\n\n";
  const auto strict = feed(parser, source, false);
  const auto strictIdentity = strict.document->blocks.front().identity;
  const size_t strictBytes = parser.committedBytes();
  source += "[ref]\n\n";
  while (source.size() < 96 * 1024) {
    source += "Normal paragraph text with **strong** wording.\n\n";
    const auto next = feed(parser, source, false);
    if (!scan::sameDocument(*next.document, *parseMarkdown(source).document) ||
        next.document->settledBlocks != 1 || next.document->blocks.front().identity != strictIdentity) return false;
    for (size_t i = 1; i < next.document->blocks.size(); i++) {
      if (next.document->blocks[i].identity != 0) return false;
    }
  }
  const auto counts = parser.counters();
  if (strictIdentity == 0 || parser.committedBytes() != strictBytes || parser.reusedBytes() < 90 * 1024 ||
      counts.fullParses != 0 || counts.fallbackCount(ParseFallback::TailLimit) != 0 ||
      counts.bytesParsed > source.size() * 5) return false;
  const auto held = parser.document();
  const auto heldSize = held->blocks.size();
  source += "[ref]";
  feed(parser, source, false);
  source += ": /late\n";
  const auto resolved = feed(parser, source, false);
  if (!scan::sameDocument(*resolved.document, *parseMarkdown(source).document) ||
      resolved.document->blocks.front().identity != strictIdentity || parser.reusedBytes() != strictBytes ||
      held->blocks.size() != heldSize || held->blocks[1].text != u"[ref]") return false;

  // Replacement/reset must release dependencies and old provisional storage.
  parser.replace("[new]" + padding + "tail");
  auto replacement = parser.parse(false);
  if (!scan::sameDocument(*replacement.document, *parseMarkdown(parser.source()).document)) return false;
  parser.append(" more\n\n");
  parser.parse(false);
  if (parser.reusedBytes() == 0 || parser.committedBytes() != 0) return false;
  parser.reset();
  const auto reset = feed(parser, "[new]" + padding + "tail", false);
  if (!scan::sameDocument(*reset.document, *parseMarkdown(parser.source()).document) ||
      parser.reusedBytes() == 0 || parser.committedBytes() != 0) return false;

  // Changing parse flags must rescan old bytes, including markers that were
  // harmless before bracket math was enabled, and reparse even without input.
  IncrementalParser changed(ParseMode::IncrementalTail, ParseOptions{0});
  const std::string mathSource = "[ref]" + padding + "\\[\n" + std::string(160, 'm') + "\n\n";
  feed(changed, mathSource, false);
  if (changed.reusedBytes() == 0) return false;
  changed.setParse(ParseOptions{});
  auto changedResult = changed.parse(false);
  if (changed.reusedBytes() != 0 ||
      !scan::sameDocument(*changedResult.document, *colored(parseMarkdown(mathSource)))) return false;
  changed.append("\\]\n");
  changedResult = changed.parse(false);
  if (!scan::sameDocument(*changedResult.document, *colored(parseMarkdown(changed.source())))) return false;
  changed.setParse(ParseOptions{0});
  changedResult = changed.parse(false);
  if (!scan::sameDocument(*changedResult.document, *parseMarkdown(changed.source(), ParseOptions{0}).document)) return false;
  changed.setParse(ParseOptions{});
  changedResult = changed.parse(false);
  if (changed.reusedBytes() != 0 ||
      !scan::sameDocument(*changedResult.document, *colored(parseMarkdown(changed.source())))) return false;

  // Opting into preview revokes provisional storage before any repair runs.
  parser.setPreview(remend::PreviewOptions::all());
  const auto previewed = parser.parse(false);
  IncrementalParser full(ParseMode::FullParse);
  full.setPreview(remend::PreviewOptions::all());
  full.append(parser.source());
  const auto fullPreview = full.parse(false);
  return scan::sameDocument(*previewed.document, *fullPreview.document) && parser.reusedBytes() == 0;
}

// What a pass returns is what a session hands every view, so it has to be the
// snapshot the parser recorded, carrying both of its flags: a stream still
// arriving, and how many leading blocks have settled. Each path is fed in turn:
// the first full parse, a tail parse after a commit, the open-fence fast path,
// and the finishing parse.
bool snapshotsCarryTheirFlags(ParseMode mode) {
  IncrementalParser parser(mode);
  const std::string prose = std::string(200, 'a') + "\n\n# Heading\n\nactive";
  const std::vector<std::string> sources = {
      prose,
      prose + " tail",
      prose + " tail\n\n```ts\nconst a = 1;\n",
      prose + " tail\n\n```ts\nconst a = 1;\nconst b = 2;\n",
  };
  bool ok = true;
  uint32_t settled = 0;
  for (const std::string &source : sources) {
    const ParseResult result = feed(parser, source, false);
    ok = ok && result.status == ParseStatus::Ok && result.document == parser.document() &&
        result.document->streaming;
    if (ok) settled = std::max(settled, result.document->settledBlocks);
  }
  const ParseResult last = feed(parser, sources.back() + "```\n", true);
  ok = ok && last.status == ParseStatus::Ok && last.document == parser.document() && !last.document->streaming &&
      last.document->settledBlocks == last.document->blocks.size();
  // The incremental parser commits the leading paragraph and heading, which is
  // what a control may act on before the stream finishes.
  return ok && (mode == ParseMode::FullParse || settled > 0);
}

} // namespace

int main(int argc, char **argv) {
  if (argc > 1 && std::string(argv[1]) == "--references") {
    CHECK(inlineCodeBracketsAndReferences(), "inline code brackets preserve commitment and streamed reference resolution");
    CHECK(referenceSafeReuse(), "reference-aware provisional reuse stays equivalent and bounded");
    return ds_check_result();
  }
  if (argc > 1 && std::string(argv[1]) == "--batch") {
    // `--batch <parseFlags>` sweeps the corpus under named parse options, which
    // is how a document with the opt-in tags on is held to the same
    // incremental-equals-full rule as every other source.
    ParseOptions parse = testdump::parseFlagsFromArg(argc, argv, 2);
    return batchedCorpus(parse);
  }
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
  CHECK(unchangedTailCodeIsCached(), "unchanged tail code is cached in both parse modes");
  CHECK(fullModeKeepsIdentities(), "full mode keeps identities");
  CHECK(snapshotsCarryTheirFlags(ParseMode::IncrementalTail), "a pass returns the snapshot it flagged (incremental)");
  CHECK(snapshotsCarryTheirFlags(ParseMode::FullParse), "a pass returns the snapshot it flagged (full)");
  CHECK(referenceSafeReuse(), "reference-aware provisional reuse stays equivalent and bounded");
  CHECK(inlineCodeBracketsAndReferences(), "inline code brackets preserve commitment and streamed reference resolution");
  CHECK(fullParseOracleSettles(), "the full-parse oracle settles code with a zero highlight budget");
  return ds_check_result();
}
