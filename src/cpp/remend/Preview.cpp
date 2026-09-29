#include "Preview.h"

#include "Compare.h"
#include "Scan.h"

#include <algorithm>
#include <bit>

namespace downstream::remend {
namespace {

using scan::isMarkdownSpace;
using scan::isWordish;
using scan::sameBlock;

/// Probes one tail may cost: each is a parse of the whole tail. Four covers
/// remend's deepest nesting case, `**bold *italic `code ~~strike`, and bounds a
/// tail full of markers that all fail.
constexpr uint64_t kMaxProbes = 4;

/// A run of marker characters that md4c left as literal text, which is what an
/// opener looks like while a stream is still arriving. A completed span has no
/// literal run left, so it is never a candidate.
struct OpenSpan {
  char16_t marker{0};
  size_t start{0};
  size_t length{0};
};

/// Only these blocks carry inline spans a repair could act on. A code block or
/// an HTML block is verbatim, and a rule has no content to style.
bool carriesInlineSpans(const Block &block) {
  return block.kind == BlockKind::Paragraph || block.kind == BlockKind::Heading ||
      block.kind == BlockKind::TableRow;
}

bool inCodeSpan(const Block &block, size_t index) {
  for (const InlineRun &run : block.runs) {
    if (run.start <= index && index < run.end && (run.styles & InlineStyle::Code) != 0) {
      return true;
    }
  }
  return false;
}

/// Every literal marker run in `block` that could be an opener md4c has not
/// closed yet, last first. The last is the innermost, so `**bold *ital` offers
/// the `*` before the `**`, and closers are appended in the order that nests.
///
/// This only proposes candidates. Nothing here decides the answer: the caller
/// has to prove each proposal by parsing it, which is also what rejects an
/// escaped marker. The block text has already had its escapes removed, so a
/// backslash in it is a literal one and says nothing about the marker after it.
std::vector<OpenSpan> findOpenSpans(const Block &block) {
  const std::u16string &text = block.text;
  std::vector<OpenSpan> found;
  size_t index = 0;
  while (index < text.size()) {
    const char16_t marker = text[index];
    if (marker != u'*' && marker != u'_' && marker != u'~' && marker != u'`') {
      index++;
      continue;
    }
    size_t end = index;
    while (end < text.size() && text[end] == marker) {
      end++;
    }
    const size_t length = end - index;

    // A marker inside a completed code span is content, not syntax.
    if (!inCodeSpan(block, index) && length >= 1 && length <= 3 && end < text.size()) {
      // A single tilde is never an opener. md4c's GitHub dialect reads `~x~`
      // as strikethrough, so closing one would invent a span the writer did not
      // type; `~~` is the delimiter the writer has to mean.
      const bool tildePair = marker != u'~' || length == 2;
      // An opener has to be followed by content, and not by whitespace, or it
      // is a list marker, a rule, or a bare delimiter with nothing to style.
      const char16_t after = text[end];
      const char16_t before = index > 0 ? text[index - 1] : u'\0';
      const bool leftFlanking = !isMarkdownSpace(after) && after != u'\0';
      // A word character on both sides makes it intraword. CommonMark lets `*`
      // open there, but `2*3` is arithmetic far more often than emphasis.
      const bool intraword = isWordish(before) && isWordish(after);
      if (tildePair && leftFlanking && !intraword) {
        found.push_back(OpenSpan{marker, index, length});
      }
    }
    index = end;
  }
  std::reverse(found.begin(), found.end());
  return found;
}

struct TrailingWhitespace {
  /// Byte offset where the region's trailing whitespace begins.
  size_t start{0};
  size_t lineBreaks{0};
};

TrailingWhitespace trailingWhitespace(std::string_view region) {
  TrailingWhitespace trailing{region.size(), 0};
  while (trailing.start > 0) {
    const char c = region[trailing.start - 1];
    if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
      break;
    }
    trailing.lineBreaks += c == '\n' ? 1 : 0;
    trailing.start--;
  }
  return trailing;
}

/// True when md4c has already closed `last`, so no later input can close a span
/// inside it. Styling it would show something the settled document never will,
/// and the style would vanish as soon as the next block starts.
///
/// A blank line ends a paragraph, and a heading or table row ends with its line.
/// A paragraph followed by one line break can still continue, so it stays open.
bool closedBy(const TrailingWhitespace &trailing, const Block &last) {
  if (trailing.lineBreaks >= 2) {
    return true;
  }
  return trailing.lineBreaks == 1 && (last.kind == BlockKind::Heading || last.kind == BlockKind::TableRow);
}

/// Inline style coverage: every UTF-16 unit counted once per style it carries.
/// A repair has to increase this, or it styles nothing and the literal text was
/// the better answer. Counting styles rather than units is what lets an outer
/// span prove itself over text an inner repair already styled, as in `**_x`.
size_t styleCoverage(const Block &block) {
  size_t total = 0;
  for (const InlineRun &run : block.runs) {
    total += static_cast<size_t>(std::popcount(run.styles)) * (run.end - run.start);
  }
  return total;
}

bool sameContainers(const Block &a, const Block &b) {
  if (a.containers.size() != b.containers.size()) {
    return false;
  }
  for (size_t i = 0; i < a.containers.size(); i++) {
    if (a.containers[i].kind != b.containers[i].kind || a.containers[i].id != b.containers[i].id ||
        a.containers[i].showsMarker != b.containers[i].showsMarker ||
        a.containers[i].task != b.containers[i].task || a.containers[i].marker != b.containers[i].marker) {
      return false;
    }
  }
  return true;
}

bool sameCells(const Block &a, const Block &b) {
  if (a.cells.size() != b.cells.size()) {
    return false;
  }
  for (size_t i = 0; i < a.cells.size(); i++) {
    if (a.cells[i].start != b.cells[i].start || a.cells[i].end != b.cells[i].end ||
        a.cells[i].align != b.cells[i].align) {
      return false;
    }
  }
  return true;
}

bool sameLinks(const Document &a, const Document &b) {
  if (a.links.size() != b.links.size()) {
    return false;
  }
  for (size_t i = 0; i < a.links.size(); i++) {
    if (a.links[i].url != b.links[i].url) {
      return false;
    }
  }
  return true;
}

/// True when `after` is the parse `before` would have produced had the span at
/// `span` really been closed, and no more than that.
///
/// The text check is the proof. A repair that changed a word, added a block, or
/// moved a container fails it, and an unproven repair is dropped rather than
/// guessed at.
bool provesRepair(const ParseResult &before, const ParseResult &after, const OpenSpan &span) {
  if (before.status != ParseStatus::Ok || after.status != ParseStatus::Ok) {
    return false;
  }
  const auto &was = before.document->blocks;
  const auto &now = after.document->blocks;
  if (was.empty() || was.size() != now.size()) {
    return false;
  }
  for (size_t i = 0; i + 1 < was.size(); i++) {
    if (!sameBlock(was[i], now[i])) {
      return false;
    }
  }

  const Block &last = was.back();
  const Block &repaired = now.back();
  if (!carriesInlineSpans(last) || !carriesInlineSpans(repaired)) {
    return false;
  }
  if (repaired.kind != last.kind || repaired.headingLevel != last.headingLevel ||
      repaired.tableHeader != last.tableHeader || repaired.table != last.table ||
      repaired.spacingBefore != last.spacingBefore || repaired.language != last.language) {
    return false;
  }
  if (!sameContainers(last, repaired) || !sameCells(last, repaired)) {
    return false;
  }
  if (!sameLinks(*before.document, *after.document)) {
    return false;
  }

  // Exactly the claimed marker run leaves the text, and nothing else moves.
  if (last.text.size() != repaired.text.size() + span.length || span.start + span.length > last.text.size()) {
    return false;
  }
  for (size_t i = span.start; i < span.start + span.length; i++) {
    if (last.text[i] != span.marker) {
      return false;
    }
  }
  for (size_t i = 0; i < span.start; i++) {
    if (last.text[i] != repaired.text[i]) {
      return false;
    }
  }
  for (size_t i = span.start; i < repaired.text.size(); i++) {
    if (repaired.text[i] != last.text[i + span.length]) {
      return false;
    }
  }

  return styleCoverage(repaired) > styleCoverage(last);
}

} // namespace

PreviewResult Preview::apply(std::string_view region, size_t origin, size_t frozenBlocks) const {
  PreviewResult result;
  result.text.assign(region);
  const ParseResult baseline = parseMarkdownAt(region, origin);
  result.parsed = baseline;
  result.bytesParsed = region.size();
  if (!options_.enabled() || baseline.status != ParseStatus::Ok ||
      baseline.document->blocks.size() <= frozenBlocks) {
    return result;
  }

  const Block &last = baseline.document->blocks.back();
  if (!carriesInlineSpans(last)) {
    return result;
  }

  // A closer after whitespace is not right-flanking, so the closer goes before
  // the trailing run. A chunk boundary often lands on a space, and without this
  // the span would flicker literal between two styled frames.
  const TrailingWhitespace trailing = trailingWhitespace(region);
  if (closedBy(trailing, last)) {
    return result;
  }
  size_t insertAt = trailing.start;

  // Innermost first. A candidate that fails its proof, such as a `*` the writer
  // escaped, is skipped rather than ending the pass, so it cannot hide a real
  // opener before it. Each proven closer becomes the baseline for the next, so
  // `~~strike **bold` closes both spans, and a candidate before the one just
  // closed keeps its offset because only text after it moved.
  ParseResult current = baseline;
  std::string text(region);
  for (const OpenSpan &span : findOpenSpans(last)) {
    if (result.stats.probes == kMaxProbes) {
      break;
    }
    const bool isCode = span.marker == u'`';
    if (isCode ? !options_.has(PreviewFeature::InlineCode) : !options_.has(PreviewFeature::Emphasis)) {
      continue;
    }
    std::string probe(text);
    probe.insert(insertAt, span.length, static_cast<char>(span.marker));
    result.stats.probes++;
    result.bytesParsed += probe.size();
    ParseResult probed = parseMarkdownAt(probe, origin);
    if (!provesRepair(current, probed, span)) {
      continue;
    }
    result.stats.applied++;
    insertAt += span.length;
    text = std::move(probe);
    current = std::move(probed);
  }

  if (result.stats.applied == 0) {
    result.stats.refused += result.stats.probes > 0 ? 1 : 0;
    return result;
  }
  result.repaired = true;
  result.text = std::move(text);
  result.parsed = std::move(current);
  return result;
}

} // namespace downstream::remend
