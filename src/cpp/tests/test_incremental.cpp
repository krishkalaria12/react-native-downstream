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

/// Where an identity was first seen, what it named, and whether any snapshot
/// showing it had settled that block.
struct SeenIdentity {
  size_t index;
  Block block;
  bool settled;
};
using SeenIdentities = std::map<uint64_t, SeenIdentity>;

/// An identity must always name the same block content, source range, and
/// position, since renderers reuse layout by identity alone.
bool stableIdentities(const Document &document, SeenIdentities &seen) {
  // A settled block keeps its identity in every later snapshot. A provisional
  // one can be revoked, but its identity is never given to other content.
  for (const auto &[identity, entry] : seen) {
    if (entry.settled && (entry.index >= document.blocks.size() || document.blocks[entry.index].identity != identity)) {
      return false;
    }
  }
  for (size_t i = 0; i < document.blocks.size(); i++) {
    const Block &block = document.blocks[i];
    if (block.identity == 0) continue;
    const auto [entry, added] = seen.try_emplace(block.identity, SeenIdentity{i, block, false});
    if (!added && (entry->second.index != i || !equalBlock(entry->second.block, block) ||
                   !(entry->second.block.sourceRange == block.sourceRange))) return false;
    entry->second.settled = entry->second.settled || i < document.settledBlocks;
  }
  return true;
}

/// Full mode carries identities for unchanged leading blocks. A block that
/// later changes loses its identity, but an identity still names one content,
/// source range, and position, and identified blocks form a prefix that ends
/// after a top-level block outside a continuing table.
bool carriedIdentities(const Document &document, SeenIdentities &seen) {
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
    const auto [entry, added] = seen.try_emplace(block.identity, SeenIdentity{i, block, false});
    if (!added && (entry->second.index != i || !equalBlock(entry->second.block, block) ||
                   !(entry->second.block.sourceRange == block.sourceRange))) return false;
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
  return highlightSettledCodeBlocks(downstreamHighlighter(), result.document, budgetMs, nullptr);
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
  const DocumentHandle partial = highlightCodeBlocks(downstreamHighlighter(), parsed.document, 0.0, nullptr);
  return actual.status == ParseStatus::Ok && !equal(*partial, *actual.document) &&
      equal(*colored(parsed, 0.0), *actual.document);
}

/// `parse` is the parse options every parse in this case runs under, so a
/// corpus can be swept with inline HTML or an opt-in syntax on as well as off.
bool check(const std::vector<std::string> &chunks, const char *label, ParseOptions parse = ParseOptions()) {
  IncrementalParser incremental(ParseMode::IncrementalTail, parse);
  IncrementalParser full(ParseMode::FullParse, parse);
  std::string source;
  SeenIdentities identities;
  SeenIdentities fullIdentities;
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
      parser.counters().plainParagraphFastUpdates == 0) return false;
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
      parser.counters().fallbackCount(ParseFallback::TailLimit) != 0 ||
      parser.counters().tailParses == 0) return false;
  source += "y";
  const auto repeated = feed(parser, source, false);
  return repeated.status == ParseStatus::Ok && repeated.document->blocks[0].identity == identity &&
      repeated.document->blocks[0].sourceRange == range &&
      parser.counters().fallbackCount(ParseFallback::TailLimit) == 0 &&
      parser.counters().plainParagraphFastUpdates > 0 &&
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
        const auto full = highlightSettledCodeBlocks(downstreamHighlighter(), parsedFull.document, 1e9, nullptr);
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
      const auto full = highlightSettledCodeBlocks(downstreamHighlighter(), parseMarkdown(source).document, 1e9, nullptr);
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
      const auto resetFull = highlightSettledCodeBlocks(downstreamHighlighter(), parseMarkdown(parser.source()).document, 1e9, nullptr);
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
// every later snapshot. A text chunk never splits a quote, and splits a list
// only between two of its top-level items, which is also where the committed
// prefix may end while the list is still arriving.
bool committedSegmentsAreStable(ParseMode mode) {
  std::string text;
  for (int i = 0; i < 40; i++) {
    text += "Paragraph " + std::to_string(i) + " has enough words to be worth committing on its own line.\n\n";
    if (i % 7 == 3) text += "> quoted\n> lines\n\n- one\n- two\n\n| a | b |\n|---|---|\n| 1 | 2 |\n\n";
    if (i % 9 == 5) {
      for (int item = 1; item <= 24; item++) {
        text += std::to_string(item) + ". **Item " + std::to_string(item) + ".** A list item long enough to commit on its own.\n";
      }
      text += "\n";
    }
  }
  IncrementalParser parser(mode);
  std::map<std::pair<uint64_t, size_t>, size_t> sealed;
  std::string source;
  size_t committedSegments = 0;
  // Chunks sealed inside a long list, which a mode that commits list items
  // produces and full mode, committing nothing, does not.
  size_t listChunks = 0;
  for (const auto &chunk : splitChunks(text, 3)) {
    source += chunk;
    const auto result = feed(parser, source, false);
    if (result.status != ParseStatus::Ok) return false;
    const Document &document = *result.document;
    const auto plan = planSegments(document);
    size_t completed = 0;
    while (completed < document.blocks.size() && document.blocks[completed].identity != 0) completed++;
    for (size_t i = 0; i < plan.size(); i++) {
      const SegmentPlan &segment = plan[i];
      if (!segment.committed) continue;
      committedSegments++;
      // Text chunks also end where a table or a code block starts, or between
      // two items of a top-level list; any other end is top-level.
      const bool beforeTable = segment.to < document.blocks.size() &&
          document.blocks[segment.to].kind == BlockKind::TableRow;
      const Block &last = document.blocks[segment.to - 1];
      const bool beforeItem = segment.to < document.blocks.size() && !last.containers.empty() &&
          last.containers[0].kind == ContainerKind::ListItem && !document.blocks[segment.to].containers.empty() &&
          document.blocks[segment.to].containers[0].showsMarker &&
          document.blocks[segment.to].containers[0].id != last.containers[0].id;
      if (!segment.isTable() && !beforeTable && !beforeItem && !last.containers.empty()) return false;
      listChunks += beforeItem && segment.to != completed;
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
  return committedSegments > 0 && sealed.size() > 2 && (mode == ParseMode::FullParse || listChunks > 0);
}

std::string numberedItems(int from, int to, const std::string &delimiter = ".") {
  std::string text;
  for (int item = from; item <= to; item++) {
    text += std::to_string(item) + delimiter + " **Point " + std::to_string(item) +
        ".** A sentence long enough that the item is worth committing, with *emphasis* and `code`.\n";
  }
  return text;
}

// Items of a list that is still arriving are committed once the next item
// starts, and the tail is parsed as the same list's continuation. Every
// snapshot still equals a full parse: numbering, tightness turning loose part
// way, a marker change starting a new list, lazy lines, nested blocks, and
// items that are not what they look like.
bool openListItems() {
  const std::string intro = "Intro paragraph that commits on its own before the list starts.\n\n";
  const std::string tight = intro + numberedItems(1, 24) + "\nAfter the list.\n\n";
  const std::string turnsLoose = "## Heading\n\n" + numberedItems(1, 5) +
      "6. **Materials.** Stone and timber.\n\n   | Material | Use |\n   |---|---|\n   | Timber | Easy |\n\n" +
      numberedItems(7, 12) + "\n## Next\n\n" + numberedItems(13, 20) + "21. " + std::string(150, 'w') +
      // A `$$` line alone opens a formula, though it looks like the line that
      // closes one, so the commit after the list must not stop there. The long
      // item makes that line a candidate on its own.
      "\n\n$$\nH \\approx \\frac{wL^2}{8f}\n$$\n\n```text\nset out arch profile\n```\n\nDone.\n";
  const std::string bullets = "Start.\n\n- first bullet item with enough words to matter here\n"
      "- second bullet item with enough words to matter here\n"
      "  - nested item under the second one, also long enough\n"
      "  - another nested item that keeps the nested list going\n"
      "- third bullet item that follows a nested list closing\n"
      "* a star bullet starts a new list right after the dashes\n"
      "* and a second star item continues that new list here\n"
      "+ then a plus list item, which is a third list again\n\nEnd.\n";
  const std::string delimiters = "Lead.\n\n" + numberedItems(1, 4, ")") +
      "5. a dot item cannot interrupt, so it is lazy\n" + numberedItems(6, 9, ")") +
      "1. but a dot list starting at one does interrupt it now\n" + numberedItems(2, 5) + "\nTail.\n";
  const std::string blocks = "Intro.\n\n" + numberedItems(1, 3) +
      "4. **Code.** An item with a fence:\n\n   ```ts\n   const a = 1;\n\n   const b = 2;\n   ```\n" +
      "5. **Quote.** An item with a quote\n   > quoted line\n   > another quoted line\n" +
      " 6. indented by one space, still a sibling item of the list\n" +
      "  7. indented by two spaces, still a sibling item too\n" +
      "   8. indented by three spaces, the last that can be\n" + numberedItems(9, 12) +
      "13.\n    text on the next line after an empty marker line\n14.\n" +
      "15. lazy\ncontinuation line without indent\n16. [link](https://example.com) item\n" +
      "- [ ] a task in a new bullet list\n- [x] a done task item in the same list\n\n* * *\n\nAfter.\n";
  std::string looseFromStart = "Intro.\n\n";
  for (int item = 1; item <= 12; item++) {
    looseFromStart += "- Loose item " + std::to_string(item) + " separated from the next one by a blank line.\n\n";
  }
  looseFromStart += "Paragraph.\n";
  const std::string numbers = "Zero.\n\n" + numberedItems(0, 4) + "\nBig.\n\n" + numberedItems(999999995, 999999999) +
      "\nAfter.\n\n" + numberedItems(8, 12) + "---\n\n" + numberedItems(1, 3) + "- a bullet ends it\n";
  const std::string setext = "Intro.\n\n" + numberedItems(1, 4) + "plain lazy text\n---\n\n" + numberedItems(1, 4) +
      "   ---\n" + numberedItems(5, 6) + "\n\n\n" + numberedItems(7, 8);
  const std::vector<std::pair<const char *, std::string>> documents = {
      {"tight list", tight}, {"list turns loose", turnsLoose}, {"bullets", bullets},
      {"delimiters", delimiters}, {"blocks in items", blocks}, {"loose from start", looseFromStart},
      {"numbers", numbers}, {"setext and breaks", setext}};
  for (const auto &[label, text] : documents) {
    for (uint32_t seed = 1; seed <= 6; seed++) {
      if (!check(splitChunks(text, seed), label)) return false;
    }
    std::vector<std::string> bytes;
    for (char byte : text) bytes.emplace_back(1, byte);
    if (!check(bytes, label)) return false;
    if (!check(splitChunks(text, 99), label, ParseOptions(ParseFlags::Enabled))) return false;
  }

  // The tail stays one or two items long through a long list, and the items
  // committed while it is open never count as settled.
  IncrementalParser parser(ParseMode::IncrementalTail);
  std::string source;
  size_t largestTail = 0;
  for (const auto &chunk : splitChunks(intro + numberedItems(1, 40), 5)) {
    source += chunk;
    const auto result = feed(parser, source, false);
    if (result.status != ParseStatus::Ok) return false;
    const Document &document = *result.document;
    if (document.settledBlocks > 1) {
      std::cerr << "An item of an open list was settled\n";
      return false;
    }
    if (source.size() > intro.size() + 1000) largestTail = std::max(largestTail, document.blocks.tail().size());
  }
  if (largestTail == 0 || largestTail > 3) {
    std::cerr << "Open list tail held " << largestTail << " blocks\n";
    return false;
  }
  source += "\nAfter the list, a paragraph long enough to commit on its own once a blank line follows.\n\nMore.\n";
  const auto closed = feed(parser, source, false);
  return closed.status == ParseStatus::Ok && closed.document->settledBlocks >= 41;
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
    } else if (heldBlock.identity == 0 || held->settledBlocks != 0 || parser.committedBytes() != 0 ||
               parser.reusedBytes() != source.size()) {
      // Provisionally shared: identified for layout reuse, but never settled.
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

    SeenIdentities identities;
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
// a provisional block is identified so renderers keep its layout, yet never
// becomes available as settled text, and revoking it never reuses an identity.
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
    SeenIdentities identities;
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
  SeenIdentities provisional;
  uint64_t referenceIdentity = 0;
  while (source.size() < 96 * 1024) {
    source += "Normal paragraph text with **strong** wording.\n\n";
    const auto next = feed(parser, source, false);
    if (!scan::sameDocument(*next.document, *parseMarkdown(source).document) ||
        next.document->settledBlocks != 1 || next.document->blocks.front().identity != strictIdentity ||
        !stableIdentities(*next.document, provisional)) return false;
    // Identified blocks form a prefix, and only the last few paragraphs, which
    // no commit attempt has reached yet, are rebuilt by a renderer.
    size_t identified = 0;
    while (identified < next.document->blocks.size() && next.document->blocks[identified].identity != 0) identified++;
    for (size_t i = identified; i < next.document->blocks.size(); i++) {
      if (next.document->blocks[i].identity != 0) return false;
    }
    if (identified + 4 < next.document->blocks.size()) return false;
    // The reference paragraph keeps one identity while it is provisional.
    if (identified > 1) {
      if (referenceIdentity == 0) referenceIdentity = next.document->blocks[1].identity;
      if (next.document->blocks[1].identity != referenceIdentity) return false;
    }
  }
  if (referenceIdentity == 0) return false;
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
      held->blocks.size() != heldSize || held->blocks[1].text != u"[ref]" ||
      held->blocks[1].identity != referenceIdentity || resolved.document->blocks[1].identity == referenceIdentity ||
      !stableIdentities(*resolved.document, provisional)) return false;

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

// Previously confirmed definitions are replayed without reparsing ordinary
// committed prose. All comparisons include images, tags, and source positions.
bool definitionContextReuse() {
  const std::string padding = "\n\n" + std::string(160, 'p') + "\n\n";
  const std::vector<std::string> cases = {
      "[ref]: https://example.test\n" + padding + "[ref] [label][ref] [ref][] ![alt][ref] ![ref]\n\nnext",
      "[Ref]: /first\n[REF]: /second\n" + padding + "[ref] [REF][] [label][rEf]\n\n[ref]: /third\n\n[ref]",
      "[two\n words]:\n /target\n \"multiline title\"\n" + padding + "[two words] ![two words]\n\nnext",
      "[a\\*b]: /escaped\n" + padding + "[a\\*b] [a*b]\n\nnext",
      "[Café]: https://example.test\n" + padding + "[CAFÉ] [label][café] ![Café]\n\nnext",
      "[ref]: <https://example.test/image.png> 'title'\n" + padding + "[![alt][ref]][ref]\n\nnext",
      "[ref]: /target\n" + padding + "[missing]\n\n[ref]\n\n[missing]: /late\n\n[missing]",
      "[ref]: /target\n" + padding + "[^note]\n\n[^note]: footnote content\n    continuation\n",
      "[ref]: /target\n" + padding + "[ref]\n\n```\n[ref]: /code\n```\n\n[ref]",
      "[ref]: /target\n" + padding + "literal ]: text\n\n[ref]",
      "[ref]: /target\n" + padding + "\\[\n" + std::string(160, 'm') + "\n\n\\]\n",
      "[ref]: /target\n" + padding + "<div>\n[ref]: /html\n</div>\n\n[ref]",
      "[ref]: /target\n" + padding + "> [other]: /quoted\n> [other]\n\n[ref] [other]",
  };
  for (const auto &text : cases) {
    IncrementalParser parser(ParseMode::IncrementalTail);
    SeenIdentities identities;
    DocumentHandle retained;
    Document copy;
    std::string source;
    for (char byte : text) {
      source.push_back(byte);
      const auto result = feed(parser, source, false);
      const auto expectedParse = parseMarkdown(source);
      if (result.status != expectedParse.status) return false;
      if (result.status != ParseStatus::Ok) continue;
      const auto expected = colored(expectedParse);
      if (!scan::sameDocument(*result.document, *expected) ||
          !stableIdentities(*result.document, identities)) {
        std::cerr << "Definition context mismatch at " << source.size() << " bytes in " << text << "\n";
        return false;
      }
      if (!retained && parser.committedBytes() != 0 && !result.document->blocks.empty()) {
        retained = result.document;
        copy = *retained;
      }
    }
    if (retained && !scan::sameDocument(*retained, copy)) return false;
    const auto done = parser.parse(true);
    if (!scan::sameDocument(*done.document, *colored(parseMarkdown(source)))) return false;
  }

  IncrementalParser parser(ParseMode::IncrementalTail);
  std::string source = "[ref]: https://example.test\n\n";
  const std::string paragraph = "Ordinary paragraph with **strong** words.\n\n";
  for (size_t i = 0; i < 2300; i++) {
    source += paragraph;
    const auto result = feed(parser, source, false);
    if (result.status != ParseStatus::Ok) return false;
  }
  const auto counts = parser.counters();
  if (parser.committedBytes() < 90 * 1024 || counts.tailParses == 0 || counts.fullParses > 4 ||
      counts.bytesParsed > source.size() * 10 ||
      !scan::sameDocument(*parser.document(), *parseMarkdown(source).document)) return false;
  // New definitions resolve old provisional text; reset/replacement never
  // inherit replayed definitions from the previous canonical source.
  source += "[ref] [new]";
  feed(parser, source, false);
  source += "\n\n[new]: /resolved\n\n";
  if (!scan::sameDocument(*feed(parser, source, false).document, *parseMarkdown(source).document)) return false;
  parser.replace("[ref]" + padding + "tail");
  if (!scan::sameDocument(*parser.parse(false).document, *parseMarkdown(parser.source()).document)) return false;
  parser.reset();
  const auto reset = feed(parser, "[ref]" + padding + "tail", false);
  if (!scan::sameDocument(*reset.document, *parseMarkdown(parser.source()).document)) return false;

  // Preview still uses its canonical full-document proof with definitions.
  IncrementalParser preview(ParseMode::IncrementalTail), full(ParseMode::FullParse);
  source = "[ref]: /target\n" + padding + "[ref] **unfinished";
  feed(preview, source, false);
  preview.setPreview(remend::PreviewOptions::all());
  full.setPreview(remend::PreviewOptions::all());
  full.append(source);
  return scan::sameDocument(*preview.parse(false).document, *full.parse(false).document);
}

bool referenceExpansionBudgets() {
  const auto compare = [](IncrementalParser &parser, std::string &source, std::string_view added) {
    source.append(added);
    const auto result = feed(parser, source, false);
    return result.status == ParseStatus::Ok &&
        scan::sameDocument(*result.document, *parseMarkdown(source, parser.options()).document);
  };
  IncrementalParser parser(ParseMode::IncrementalTail, ParseOptions{ParseFlags::LinkRelative});
  std::string source;
  if (!compare(parser, source, std::string(4096, 'p') + "\n\n") ||
      !compare(parser, source, "[r]: /" + std::string(1000, 'a') + "\n\n")) return false;
  for (int i = 0; i < 100; i++) if (!compare(parser, source, "[r] ")) return false;
  // Cumulative committed references consume the same global budget as a full
  // parse. A tail must not receive a fresh cap on each small paragraph.
  for (int i = 0; i < 160; i++) if (!compare(parser, source, "\n\n[r]\n\n")) return false;
  const size_t linksBefore = parser.document()->links.size();
  if (!compare(parser, source, std::string(16384, 'p') + "\n\n") ||
      parser.document()->links.size() <= linksBefore) return false;
  return scan::sameDocument(*parser.parse(true).document, *parseMarkdown(source, parser.options()).document);
}

bool plainParagraphAppendAndFallback() {
  const std::vector<std::pair<std::string, std::string>> cases = {
      {"plain words", " more words   after spaces"},
      {"Normal paragraph", " with sentences. More words.  "},
      {"www", ".example.test"}, {"plain w", "ww.example.test"}, {"plain WW", "W.example.test"}, {"https", "://example.test"}, {"1", ". list item"},
      {"plain", "\n---\n"}, {"table", " | cell\n--- | ---\n"},
      {"plain", " *styled* [ref]\n\n[ref]: /target\n"},
      {"plain", " &amp; &#0; \\*escaped\\*"}, {"plain", "  \nnext line"},
      {"plain", "\r\nnext\tline"}, {"plain", " $$x$$ \\(y\\)"},
  };
  for (const auto &[before, after] : cases) {
    IncrementalParser parser(ParseMode::IncrementalTail);
    std::string source;
    for (char byte : before + after) {
      source.push_back(byte);
      const auto result = feed(parser, source, false);
      if (!scan::sameDocument(*result.document, *colored(parseMarkdown(source)))) return false;
    }
    if ((before.size() > 1 && parser.counters().plainParagraphFastUpdates == 0) ||
        !scan::sameDocument(*parser.parse(true).document, *colored(parseMarkdown(source)))) return false;
  }
  IncrementalParser parser(ParseMode::IncrementalTail);
  const std::string chunk(128, 'p');
  std::string source;
  DocumentHandle held;
  for (size_t i = 0; i < 768; i++) {
    source += chunk;
    const auto result = feed(parser, source, false);
    if (i == 0) held = result.document;
  }
  const auto counts = parser.counters();
  return counts.plainParagraphFastUpdates == 767 && counts.bytesParsed == 128 &&
      held->blocks.front().text.size() == 128 && parser.document()->blocks.front().text.size() == source.size() &&
      scan::sameDocument(*parser.parse(true).document, *parseMarkdown(source).document);
}

bool tagMetadataSurvivesSplicing() {
  ParseOptions options{ParseFlags::InlineHtml};
  options.tags.tags = {{"user", {"id"}}};
  const std::string prefix = "café " + std::string(160, 'p') + "\n\n";
  for (const std::string &definitions : {std::string(), std::string("[r]: /target\n\n")}) {
    const std::vector<std::string> cases = {
        "<user id='a'>one</user>\n\n<user id='b'>two</user>\n\n[ref](https://example.test)",
        "<user>\n", "<user>\n</user>\n", "<user>\nbody\n</user>\n\nnext",
        "<user>\n<user>\nnested\n</user>\n</user>\n\nnext",
        "<a href='https://example.test'>link</a>\n\n<user>\nbody\n</user>\n",
    };
    for (const auto &tail : cases) {
      IncrementalParser parser(ParseMode::IncrementalTail, options);
      std::string source = definitions + prefix;
      if (feed(parser, source, false).status != ParseStatus::Ok) return false;
      for (char byte : tail) {
        source.push_back(byte);
        const auto actual = feed(parser, source, false);
        const auto expected = parseMarkdown(source, options);
        if (actual.status != expected.status || !scan::sameDocument(*actual.document, *expected.document)) {
          std::cerr << "Tag metadata mismatch at " << source.size() << " bytes in " << source << "\n";
          return false;
        }
      }
      const auto retained = parser.document();
      const Document copy = *retained;
      if (!scan::sameDocument(*parser.parse(true).document, *parseMarkdown(source, options).document) ||
          !scan::sameDocument(*retained, copy)) return false;
    }
  }
  return true;
}

// Finalization reparses the canonical tail and keeps the proven prefix's
// immutable storage. Its cost must not grow with the already settled answer.
bool canonicalFinishBoundsAndContexts() {
  ParseOptions options{ParseFlags::Default | ParseFlags::InlineHtml};
  options.tags.tags = {{"user", {"id"}}};
  std::string prefix;
  for (unsigned i = 0; i < 120; ++i) prefix += "café paragraph " + std::to_string(i) + " " + std::string(180, 'p') + "\n\n";
  prefix += "[prefix link](https://example.test)\n\n![prefix image](https://example.test/image.png)\n\n";
  prefix += "<user id='prefix'>\nbody\n</user>\n\n";
  prefix += "- list item\n\nclosing paragraph " + std::string(180, 'c') + "\n\n";
  const std::vector<std::string> tails = {
      "raw **unfinished", "[tail link](https://tail.test) ![tail image](https://tail.test/image.png)",
      "```ts title=\"file\"\nconst value = compute(42);\n", "```ts\nconst value = 42;\n```\n",
      "$$\nx + y\n", "$$x + y$$", "> quote\n> - nested item", "- first\n- second",
      "| a | b |\n| - | - |\n| 1 | 2 |", "<div>\nopen HTML\n",
      "<user id='tail'>\nbody\n", "<user id='tail'>\nbody\n</user>\n",
  };
  for (const std::string &tail : tails) {
    IncrementalParser parser(ParseMode::IncrementalTail, options);
    const auto first = feed(parser, prefix, false);
    if (first.status != ParseStatus::Ok || parser.committedBytes() != prefix.size()) return false;
    const auto live = feed(parser, prefix + tail, false);
    if (live.status != ParseStatus::Ok) return false;
    const auto retained = live.document;
    const auto prefixBlock = &retained->blocks[0];
    const size_t bytes = parser.reusedBytes();
    const auto before = parser.counters();
    const auto final = parser.parse(true);
    IncrementalParser full(ParseMode::FullParse, options);
    full.append(prefix + tail);
    const auto expected = full.parse(true);
    const auto after = parser.counters();
    if (final.status != ParseStatus::Ok || !scan::sameDocument(*final.document, *expected.document) ||
        final.document->streaming || final.document->settledBlocks != final.document->blocks.size() ||
        &final.document->blocks[0] != prefixBlock || final.document->blocks[0].identity != prefixBlock->identity ||
        after.bytesParsed - before.bytesParsed != prefix.size() + tail.size() - bytes ||
        after.fullParses != before.fullParses || after.tailParses != before.tailParses + 1 || !retained->streaming) {
      std::cerr << "Canonical finish failed or reparsed prefix in " << tail << "\n"
                << "bytes " << bytes << " delta " << after.bytesParsed - before.bytesParsed
                << " full " << before.fullParses << "->" << after.fullParses
                << " shared " << retained->blocks.sharedSize() << "->" << final.document->blocks.sharedSize()
                << " parity " << scan::sameDocument(*final.document, *expected.document) << "\n";
      return false;
    }
    for (size_t i = 0; i < final.document->blocks.size(); ++i) {
      const auto &actualMath = final.document->blocks[i].math;
      const auto &expectedMath = expected.document->blocks[i].math;
      for (size_t j = 0; j < actualMath.size(); ++j) {
        if (actualMath[j].layout != expectedMath[j].layout || actualMath[j].error != expectedMath[j].error) return false;
      }
    }
    const auto again = parser.parse(true);
    const auto repeated = parser.counters();
    if (again.document != final.document || repeated.bytesParsed != after.bytesParsed ||
        repeated.blocksHighlighted != after.blocksHighlighted || repeated.formulasLaidOut != after.formulasLaidOut) return false;
  }
  return true;
}

bool canonicalFinishFallbacks() {
  const std::string prefix = std::string(200, 'p') + "\n\n";
  const std::vector<std::string> dependencies = {
      "[ref]\n\n[ref]: /target\n", "footnote[^n]\n\n[^n]: body\n", "\\(x + y\\)",
      "\\[\nx + y\n\\]\n", "[unresolved]\n\n" + std::string(200, 'q') + "\n\nactive",
  };
  for (const std::string &tail : dependencies) {
    IncrementalParser parser(ParseMode::IncrementalTail);
    if (feed(parser, prefix, false).status != ParseStatus::Ok ||
        feed(parser, prefix + tail, false).status != ParseStatus::Ok) return false;
    const auto before = parser.counters();
    const auto final = parser.parse(true);
    if (final.status != ParseStatus::Ok || !scan::sameDocument(*final.document, *colored(parseMarkdown(prefix + tail))) ||
        parser.counters().fullParses != before.fullParses + 1 ||
        parser.counters().fallbackCount(ParseFallback::Finished) != before.fallbackCount(ParseFallback::Finished) + 1) return false;
  }
  // Items committed from an open list are provisional. Their raw tail cannot
  // finish independently of the list lead and its final tightness.
  for (const bool ordered : {false, true}) {
    IncrementalParser parser(ParseMode::IncrementalTail);
    std::string source = prefix;
    if (feed(parser, source, false).status != ParseStatus::Ok) return false;
    for (unsigned i = 1; i <= 20; ++i) {
      source += (ordered ? std::to_string(i) + ". " : "- ") + std::string(80, 'w') + "\n";
      if (feed(parser, source, false).status != ParseStatus::Ok) return false;
    }
    if (parser.committedBytes() == 0 || parser.reusedBytes() <= parser.committedBytes() ||
        parser.document()->settledBlocks >= parser.document()->blocks.size()) return false;
    const auto before = parser.counters();
    const auto final = parser.parse(true);
    if (final.status != ParseStatus::Ok || !scan::sameDocument(*final.document, *colored(parseMarkdown(source))) ||
        parser.counters().fullParses != before.fullParses + 1 ||
        final.document->settledBlocks != final.document->blocks.size()) return false;
  }
  IncrementalParser preview(ParseMode::IncrementalTail);
  preview.setPreview(remend::PreviewOptions::all());
  feed(preview, prefix + "**unfinished", false);
  const auto beforePreview = preview.counters();
  const auto finishedPreview = preview.parse(true);
  if (!scan::sameDocument(*finishedPreview.document, *parseMarkdown(prefix + "**unfinished").document) ||
      preview.counters().fullParses != beforePreview.fullParses + 1) return false;

  // The cap applies to all formulas, including shared prefix formulas. Full
  // fallback must drop their old layouts and record the canonical global error.
  IncrementalParser capped(ParseMode::IncrementalTail);
  std::string many;
  for (unsigned i = 0; i <= math_limits::kMaxFormulas; ++i) {
    many += "$$x$$\n\n";
    if (feed(capped, many, false).status != ParseStatus::Ok) return false;
  }
  const auto beforeCap = capped.counters();
  const auto finalCap = capped.parse(true);
  IncrementalParser full(ParseMode::FullParse);
  full.append(many);
  const auto expected = full.parse(true);
  if (!scan::sameDocument(*finalCap.document, *expected.document) ||
      capped.counters().fullParses != beforeCap.fullParses + 1) return false;
  for (size_t i = 0; i < finalCap.document->blocks.size(); ++i) {
    for (size_t j = 0; j < finalCap.document->blocks[i].math.size(); ++j) {
      if (finalCap.document->blocks[i].math[j].layout != expected.document->blocks[i].math[j].layout ||
          finalCap.document->blocks[i].math[j].error != expected.document->blocks[i].math[j].error) return false;
    }
  }
  return true;
}

MathEngineResult rejectFinalMath(const char *, const MathEngineOptions *) { return {nullptr, 1}; }
void releaseFinalMath(char *) {}
const char *finalMathError() { return "The final formula is incomplete."; }

bool finalMathSettlesAndRenderingHint() {
  MathEngine::install({rejectFinalMath, releaseFinalMath, finalMathError}, "");
  bool correct = true;
  for (const ParseMode mode : {ParseMode::FullParse, ParseMode::IncrementalTail}) {
    IncrementalParser parser(mode);
    const std::string source = std::string(200, 'p') + "\n\n$$\n\\frac{a}{\n";
    const auto live = feed(parser, source, false);
    const auto before = parser.counters();
    const auto final = parser.parse(true);
    correct = correct && live.status == ParseStatus::Ok && final.status == ParseStatus::Ok &&
        live.document->blocks.back().math.size() == 1 && live.document->blocks.back().math[0].error.empty() &&
        final.document->blocks.back().math.size() == 1 &&
        final.document->blocks.back().math[0].error == finalMathError() &&
        final.document->renderReuseFrom == 0 && parser.counters().mathFailed > before.mathFailed;
    const auto after = parser.counters();
    correct = correct && parser.parse(true).document == final.document &&
        parser.counters().bytesParsed == after.bytesParsed && parser.counters().mathFailed == after.mathFailed;
  }
  MathEngine::install({}, "");
  return correct;
}

bool finishedRenderingHintRequiresProof() {
  for (const ParseMode mode : {ParseMode::FullParse, ParseMode::IncrementalTail}) {
    IncrementalParser parser(mode);
    const auto live = feed(parser, "unchanged prose", false);
    const auto final = parser.parse(true);
    if (final.document->id == live.document->id || final.document->renderReuseFrom != live.document->id) return false;
    parser.append(" changed");
    const auto changed = parser.parse(false);
    if (changed.document->renderReuseFrom != 0) return false;
    parser.setPreview(remend::PreviewOptions::all());
    parser.append(" **unfinished");
    const auto repaired = parser.parse(false);
    const auto canonical = parser.parse(true);
    if (repaired.document->blocks.back().text == canonical.document->blocks.back().text ||
        canonical.document->renderReuseFrom != 0) return false;
  }
  return true;
}

bool fullModeDropsChangedMathIdentities() {
  IncrementalParser parser(ParseMode::FullParse);
  parser.append("$$x$$\n\n$$y$$\n\n");
  if (parser.parse(false).status != ParseStatus::Ok) return false;
  parser.append("plain\n\n");
  const auto before = parser.parse(false);
  if (before.document->blocks[0].identity == 0 || !before.document->blocks[0].math[0].error.empty()) return false;
  std::string overflow;
  for (unsigned i = 0; i < math_limits::kMaxFormulas; ++i) overflow += "$$z$$\n\n";
  parser.append(overflow);
  const auto final = parser.parse(true);
  return final.status == ParseStatus::Ok && final.document->blocks[0].identity == 0 &&
      !final.document->blocks[0].math[0].error.empty() && final.document->renderReuseFrom == 0;
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
    CHECK(definitionContextReuse(), "confirmed definition replay preserves prefix parity and bounded work");
    CHECK(referenceExpansionBudgets(), "reference expansion retains canonical global budgets");
    CHECK(tagMetadataSurvivesSplicing(), "tag indices and source ranges survive tail splicing");
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
  CHECK(openListItems(), "open list items commit and the tail continues the list");
  CHECK(realisticAnswer(), "realistic answer");
  CHECK(unchangedTailCodeIsCached(), "unchanged tail code is cached in both parse modes");
  CHECK(fullModeKeepsIdentities(), "full mode keeps identities");
  CHECK(snapshotsCarryTheirFlags(ParseMode::IncrementalTail), "a pass returns the snapshot it flagged (incremental)");
  CHECK(snapshotsCarryTheirFlags(ParseMode::FullParse), "a pass returns the snapshot it flagged (full)");
  CHECK(referenceSafeReuse(), "reference-aware provisional reuse stays equivalent and bounded");
  CHECK(inlineCodeBracketsAndReferences(), "inline code brackets preserve commitment and streamed reference resolution");
  CHECK(definitionContextReuse(), "confirmed definition replay preserves prefix parity and bounded work");
  CHECK(referenceExpansionBudgets(), "reference expansion retains canonical global budgets");
  CHECK(plainParagraphAppendAndFallback(), "plain paragraph append bounds work and falls back for grammar mutations");
  CHECK(tagMetadataSurvivesSplicing(), "tag indices and source ranges survive tail splicing");
  CHECK(fullParseOracleSettles(), "the full-parse oracle settles code with a zero highlight budget");
  CHECK(canonicalFinishBoundsAndContexts(), "canonical finish parses only safe tail and preserves immutable prefix");
  CHECK(canonicalFinishFallbacks(), "canonical finish retains global dependency and formula-cap fallbacks");
  CHECK(finalMathSettlesAndRenderingHint(), "final math errors settle and repeated finish does no work");
  CHECK(finishedRenderingHintRequiresProof(), "finish rendering hint requires unchanged canonical payload");
  CHECK(fullModeDropsChangedMathIdentities(), "full mode invalidates cached identities when global math answers change");
  return ds_check_result();
}
