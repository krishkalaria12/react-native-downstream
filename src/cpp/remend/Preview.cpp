#include "Preview.h"

#include "Compare.h"
#include "Scan.h"

#include <algorithm>
#include <bit>
#include <optional>
#include <string>
#include <vector>

namespace downstream::remend {
namespace {

using scan::isMarkdownSpace;
using scan::isWordish;
using scan::sameBlock;

/// Span probes one tail may cost: each is a parse of the whole tail. Five
/// covers remend's deepest nesting case, `**bold *italic `code ~~strike`, with
/// room for one more, and bounds a tail of markers that all fail. The fragment
/// and link repairs are counted apart from this, because each of them is a
/// single decision rather than a walk over candidates.
constexpr uint64_t kMaxProbes = 5;

/// A run of marker characters that md4c left as literal text, which is what an
/// opener looks like while a stream is still arriving. A completed span has no
/// literal run left, so it is never a candidate.
struct OpenSpan {
  char16_t marker{0};
  size_t start{0};
  size_t length{0};
};

/// Only these blocks carry inline text a repair could act on. A code block, a
/// math block and an HTML block are verbatim, and a rule has no content to
/// style.
///
/// An image block counts because its text *is* its alt text, which is what an
/// unfinished link or image is repaired into. Without it, closing an unfinished
/// image would produce an image block and then be refused for producing one,
/// which is the repair that stops `![a](https://exa` from ever settling.
bool carriesInlineSpans(const Block &block) {
  return block.kind == BlockKind::Paragraph || block.kind == BlockKind::Heading ||
      block.kind == BlockKind::TableRow || block.kind == BlockKind::Image;
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

/// True when `after` has the same structure as `before`: the same blocks, every
/// block before the last identical, the last block the same kind in the same
/// containers and cells, and the same actionable links. What remains for a
/// proof is the last block's text.
/**
 * Everything two parses must share for a repair to be allowed: the block count,
 * every block but the last byte-identical, the containers, the cells, and the
 * links.
 *
 * `sameShape` is this plus the last block's kind and its level, header, table and
 * spacing. The link and math proofs each re-check the parts of the last block
 * they are allowed to change, so they start here rather than from `sameShape` and
 * subtract — a proof that has to un-do a check is a proof whose exemptions are
 * easy to get wrong.
 *
 * The image count is *not* checked here, because one repair legitimately adds an
 * image and the others may not. Each proof makes its own claim about it, which is
 * one line in each place rather than an exemption here that all three would have
 * to remember.
 */
bool sameBlocksAndText(const ParseResult &before, const ParseResult &after) {
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
  if (!carriesInlineSpans(was.back()) || !carriesInlineSpans(now.back())) {
    return false;
  }
  if (!sameContainers(was.back(), now.back()) || !sameCells(was.back(), now.back())) {
    return false;
  }
  return sameLinks(*before.document, *after.document);
}

bool sameShape(const ParseResult &before, const ParseResult &after) {
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
  return sameLinks(*before.document, *after.document);
}

/// True when `after` is the parse `before` would have produced had the span at
/// `span` really been closed, and no more than that.
///
/// The text check is the proof. A repair that changed a word, added a block, or
/// moved a container fails it, and an unproven repair is dropped rather than
/// guessed at.
bool provesRepair(const ParseResult &before, const ParseResult &after, const OpenSpan &span) {
  if (!sameShape(before, after)) {
    return false;
  }
  if (after.document->images.size() != before.document->images.size()) {
    return false;
  }
  const Block &last = before.document->blocks.back();
  const Block &repaired = after.document->blocks.back();

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

/// An unfinished link or image at the end of the tail. It is found in the
/// source rather than the block text, because the source syntax is what the
/// repair rewrites.
struct OpenLink {
  bool image{false};
  /// Offset of the `[`, or of the `!` before it for an image.
  size_t start{0};
  /// Offset where `]()` goes: the `]` of `[text](partial`, or the content end
  /// for `[text`. Everything from here to the content end is dropped, so a
  /// partial target never reaches the parse.
  size_t close{0};
};

bool isOpenBracket(std::string_view source, size_t index) {
  return source[index] == '[' && !scan::isEscaped(source, index);
}

/// The `[` that the `]` at `close` closes, or kNoOffset.
size_t matchingOpen(std::string_view source, size_t close) {
  size_t depth = 0;
  for (size_t i = close; i-- > 0;) {
    if (scan::isEscaped(source, i)) {
      continue;
    }
    if (source[i] == ']') {
      depth++;
    } else if (source[i] == '[') {
      if (depth == 0) {
        return i;
      }
      depth--;
    }
  }
  return scan::kNoOffset;
}

/// Whether the text after `](` could still become a destination. A space ends a
/// bare destination, so `[note](which is long` is prose unless a title follows
/// the space; dropping it would delete words that will never be a link.
bool couldBeDestination(std::string_view partial) {
  if (!partial.empty() && partial.front() == '<') {
    return true;
  }
  const size_t space = partial.find_first_of(" \t\r\n");
  if (space == std::string_view::npos) {
    return true;
  }
  const size_t next = partial.find_first_not_of(" \t\r\n", space);
  return next == std::string_view::npos || partial[next] == '"' || partial[next] == '\'' || partial[next] == '(';
}

/// Whether `[` at `open` is a task marker still arriving, as in `- [` or
/// `- [x`. Closing it as a link would show a letter where a checkbox is about
/// to appear.
bool looksLikeTaskMarker(std::string_view content, size_t open) {
  const std::string_view rest = content.substr(open + 1);
  if (rest.size() > 1 || (rest.size() == 1 && rest != " " && rest != "x" && rest != "X")) {
    return false;
  }
  const size_t newline = content.rfind('\n', open);
  const size_t lineStart = newline == std::string_view::npos ? 0 : newline + 1;
  const std::string_view prefix = content.substr(lineStart, open - lineStart);
  // Quote markers and indentation, then a list marker and its space.
  size_t at = prefix.find_first_not_of(" >");
  if (at == std::string_view::npos) {
    return false;
  }
  if (prefix[at] == '-' || prefix[at] == '*' || prefix[at] == '+') {
    at++;
  } else {
    const size_t digits = prefix.find_first_not_of("0123456789", at);
    if (digits == at || digits == std::string_view::npos || (prefix[digits] != '.' && prefix[digits] != ')')) {
      return false;
    }
    at = digits + 1;
  }
  return at < prefix.size() && prefix.find_first_not_of(' ', at) == std::string_view::npos;
}

std::optional<OpenLink> findOpenLink(std::string_view content) {
  const auto link = [&](size_t open, size_t close) {
    const bool image = open > 0 && content[open - 1] == '!' && !scan::isEscaped(content, open - 1);
    return OpenLink{image, image ? open - 1 : open, close};
  };

  // `[text](partial`: the last `](` whose destination has not closed. Any later
  // `](` would sit inside that destination, so the last is the only candidate.
  const size_t paren = content.rfind("](");
  if (paren != std::string_view::npos && !scan::isEscaped(content, paren) &&
      scan::skipInlineDestination(content, paren + 1) == scan::kNoOffset) {
    if (!couldBeDestination(content.substr(paren + 2))) {
      return std::nullopt;
    }
    const size_t open = matchingOpen(content, paren);
    return open == scan::kNoOffset ? std::nullopt : std::optional<OpenLink>(link(open, paren));
  }

  // `[text`: the last `[` nothing has closed. Appending `]` closes the
  // innermost, as the writer's own bracket will.
  std::vector<size_t> open;
  for (size_t i = 0; i < content.size(); i++) {
    if (isOpenBracket(content, i)) {
      open.push_back(i);
    } else if (content[i] == ']' && !scan::isEscaped(content, i) && !open.empty()) {
      open.pop_back();
    }
  }
  if (open.empty() || looksLikeTaskMarker(content, open.back())) {
    return std::nullopt;
  }
  return link(open.back(), content.size());
}

/// The runs of `block` that start before `end`, clipped to it, so two blocks
/// can be compared over a shared prefix.
std::vector<InlineRun> runsBefore(const Block &block, size_t end) {
  std::vector<InlineRun> runs;
  for (const InlineRun &run : block.runs) {
    if (run.start < end) {
      InlineRun clipped = run;
      clipped.end = std::min<uint32_t>(run.end, static_cast<uint32_t>(end));
      runs.push_back(clipped);
    }
  }
  return runs;
}

/// True when `after` shows the unfinished link in `before` as its text and
/// nothing else changed: the last block lost the opening bracket (`[` or `![`)
/// and everything after the link text, which is `](` and the partial target or
/// nothing at all, and the text before the link kept its styles. `textStart`
/// receives where the link text begins in `after`.
bool provesLinkRepair(const ParseResult &before, const ParseResult &after, bool image, size_t &textStart) {
  // `sameShape` is the whole proof except for one thing: closing an unfinished
  // image promotes the paragraph to an image block, because an image-only
  // paragraph becomes one. That is the repair working, so the kind check is
  // re-made here with the single transition this repair is allowed to cause.
  // Everything else `sameShape` compares — the earlier blocks, the containers,
  // the cells, the links, the styles — still has to hold.
  if (!sameBlocksAndText(before, after)) {
    return false;
  }
  // An unfinished image becomes a real image record; an unfinished link does not
  // become an image, and neither may change how many images there are otherwise.
  if (image ? after.document->images.size() != before.document->images.size() + 1
            : after.document->images.size() != before.document->images.size()) {
    return false;
  }
  const Block &last = before.document->blocks.back();
  const Block &repaired = after.document->blocks.back();
  const bool promoted = image && last.kind == BlockKind::Paragraph && repaired.kind == BlockKind::Image;
  if (!promoted && repaired.kind != last.kind) {
    return false;
  }

  const std::u16string &was = last.text;
  const std::u16string &now = repaired.text;
  const std::u16string_view opener = image ? u"![" : u"[";

  size_t prefix = 0;
  while (prefix < now.size() && prefix < was.size() && now[prefix] == was[prefix]) {
    prefix++;
  }
  const size_t linkText = prefix + opener.size();
  const size_t length = now.size() - prefix;
  if (std::u16string_view(was).substr(prefix, opener.size()) != opener || linkText + length > was.size() ||
      std::u16string_view(was).substr(linkText, length) != std::u16string_view(now).substr(prefix)) {
    return false;
  }
  const std::u16string_view dropped = std::u16string_view(was).substr(linkText + length);
  if (!dropped.empty() && dropped.substr(0, 2) != u"](") {
    return false;
  }
  const std::vector<InlineRun> wasRuns = runsBefore(last, prefix);
  const std::vector<InlineRun> nowRuns = runsBefore(repaired, prefix);
  if (wasRuns.size() != nowRuns.size()) {
    return false;
  }
  for (size_t i = 0; i < wasRuns.size(); i++) {
    if (wasRuns[i].start != nowRuns[i].start || wasRuns[i].end != nowRuns[i].end ||
        wasRuns[i].styles != nowRuns[i].styles) {
      return false;
    }
  }
  textStart = prefix;
  return true;
}

/**
 * An unclosed inline `$$` on the region's last line, if there is one.
 *
 * Only the double-dollar form, and only on one line, because both are facts
 * about the syntax rather than limits of the pass. A single `$` is a currency
 * symbol more often than it is a formula. And md4c resolves a dollar run within
 * one line, so a `$$` on an earlier line can never pair with a closer appended
 * here — offering that repair would probe a candidate that cannot succeed.
 *
 * `\(` never reaches this at all: the pre-scan only rewrites a *complete* pair,
 * so an unfinished `\(` is still the CommonMark escape it would be in a
 * finished document, which is the correct answer for both.
 */
size_t openMathRun(std::string_view content, const ParseOptions &math) {
  // The last line, because that is the only one a closer appended at the end of
  // the region can reach.
  const size_t lastBreak = content.rfind('\n');
  const std::string_view line =
      lastBreak == std::string_view::npos ? content : content.substr(lastBreak + 1);
  int depth = 0;
  size_t open = 0;
  for (size_t i = 0; i < line.size(); i++) {
    if (line[i] != '$' || scan::isEscaped(line, i)) {
      continue;
    }
    size_t end = i;
    while (end < line.size() && line[end] == '$') {
      end++;
    }
    // A run of three or more is neither math nor a fence. A run of one is only
    // math when the app asked for it, so whether it counts is read from the same
    // options the parse was: guessing here would cost a probe per snapshot on
    // every answer that mentions a price.
    if (end - i == 2 || (end - i == 1 && math.singleDollar())) {
      // md4c pairs a run only with a run of the same length, so the length is
      // what the closer has to be. Returning it rather than a bool is what lets
      // the repair append a single `$` to a single `$` and a `$$` to a `$$`.
      if (depth == 0) {
        open = end - i;
      }
      depth = depth == 0 ? 1 : 0;
    }
    i = end - 1;
  }
  return depth == 0 ? 0 : open;
}

/**
 * Whether closing the open `$$` changed nothing but that formula.
 *
 * Stronger than the span proofs, and deliberately: this repair *adds* a run and
 * a span, so the rule that the other repairs use — nothing but the marker
 * changed — would reject it. What is checked instead is the whole claim: the
 * block count is unchanged, every earlier block is byte-identical, the last
 * block's text is the old text plus exactly the two delimiter characters, and
 * that block now carries one display formula covering all of it. A `$$` that
 * pairs with an earlier one, or that swallows a table cell, fails every clause.
 */
bool provesMathRepair(const ParseResult &before, const ParseResult &after) {
  if (!sameBlocksAndText(before, after)) {
    return false;
  }
  if (after.document->images.size() != before.document->images.size()) {
    return false;
  }
  const Block &last = before.document->blocks.back();
  const Block &repaired = after.document->blocks.back();
  // A formula alone in its paragraph is promoted to a math block, which is a
  // different repair from this one, so the block has to stay a paragraph: this
  // claim is about a formula that arrived inside a sentence.
  if (last.kind != BlockKind::Paragraph || repaired.kind != BlockKind::Paragraph) {
    return false;
  }
  // The claim, in the order it is checked.
  //
  // The old text is a prefix of the new one, and everything the repair added
  // after it is whitespace followed by the closing `$$`. Whitespace, because
  // md4c drops a line's trailing spaces from a paragraph: `$$x + ` has a
  // baseline text of `$$x +` and a repaired text of `$$x + $$`, so the two
  // differ by the space as well as by the delimiter and a strict size check
  // would refuse the one repair this pass exists for.
  //
  // The span has to reach the end of the block, or the repair put the closer
  // somewhere other than where it claims. Its style has to follow from the
  // delimiter it was written with, which is the one fact about a formula this
  // can check without knowing what the writer meant: a doubled pair is display
  // and a single one is text style, and a span that claims otherwise did not
  // come from this source.
  if (repaired.text.size() < last.text.size() + 2 ||
      repaired.text.compare(0, last.text.size(), last.text) != 0) {
    return false;
  }
  for (size_t i = last.text.size(); i + 2 < repaired.text.size(); i++) {
    const char16_t unit = repaired.text[i];
    if (unit != u' ' && unit != u'\t' && unit != u'\n' && unit != u'\r') {
      return false;
    }
  }
  if (repaired.text[repaired.text.size() - 1] != u'$') {
    return false;
  }
  if (repaired.math.size() != 1) {
    return false;
  }
  const MathSpan &span = repaired.math.front();
  if (span.end != repaired.text.size() || span.tex.empty()) {
    return false;
  }
  const bool doubled = span.delimiter == MathDelimiter::DoubleDollar;
  return span.display == doubled && repaired.text[span.start] == '$' &&
      repaired.text[span.start + (doubled ? 1 : 0)] == '$';
}

/**
 * A half-arrived allowlisted tag at the end of the tail, if there is one.
 *
 * Found in the source rather than the block text, because the fragment is
 * syntax rather than content, and a candidate has to be the tail itself: text
 * after it has already been decided. The prefix rule is the one the matcher
 * enforces, so the two can never disagree about which tags exist.
 */
std::string_view openTagFragment(std::string_view content) {
  const size_t open = content.rfind('<');
  if (open == std::string_view::npos) {
    return {};
  }
  const std::string_view fragment = content.substr(open);
  if (!scan::isInlineTagPrefix(fragment)) {
    return {};
  }
  return fragment;
}

/**
 * Whether hiding the fragment changed nothing but that fragment.
 *
 * The same shape as the link proof: the block count is unchanged, every block
 * before the last is byte-identical, the last block's text is the old text
 * minus exactly the fragment and nothing else, and the fragment sat in no code
 * or math run, so it was syntax the parser had not finished rather than content
 * it would now swallow.
 */
bool provesTagRepair(const ParseResult &before, const ParseResult &after, std::string_view fragment) {
  if (!sameShape(before, after)) {
    return false;
  }
  if (after.document->images.size() != before.document->images.size()) {
    return false;
  }
  const Block &last = before.document->blocks.back();
  const Block &repaired = after.document->blocks.back();
  const size_t fragmentUnits = fragment.size();
  if (fragmentUnits == 0 || last.text.size() != repaired.text.size() + fragmentUnits ||
      fragmentUnits > last.text.size()) {
    return false;
  }
  // The fragment is the tail of the old text, and everything before it is the
  // text the repair kept.
  const size_t at = repaired.text.size();
  if (std::u16string_view(last.text).substr(at, fragmentUnits) != std::u16string(fragment.begin(), fragment.end())) {
    return false;
  }
  for (size_t i = 0; i < at; i++) {
    if (last.text[i] != repaired.text[i]) {
      return false;
    }
  }
  // The fragment is where the tag bytes landed, so a code span or a formula
  // covering it means the writer had already typed content, not syntax.
  for (const InlineRun &run : last.runs) {
    if (run.start < last.text.size() && run.end >= at &&
        (run.styles & (InlineStyle::Code | InlineStyle::Math)) != 0) {
      return false;
    }
  }
  return true;
}

} // namespace

PreviewResult Preview::apply(std::string_view region, size_t origin, size_t frozenBlocks, ParseOptions parse) const {
  PreviewResult result;
  result.text.assign(region);
  const ParseResult baseline = parseMarkdownAt(region, origin, parse);
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
  TrailingWhitespace trailing = trailingWhitespace(region);
  if (closedBy(trailing, last)) {
    return result;
  }
  ParseResult current = baseline;
  std::string text(region);

  // A half-arrived tag first, because it is the one fragment the writer cannot
  // have meant as content: the tag bytes have no text between them and no
  // closing `>`, so nothing after this repair can be decided from a tail that
  // still contains them. A candidate that fails its proof is not hidden, which
  // is what keeps `a<b` visible.
  if (options_.has(PreviewFeature::HtmlTags)) {
    const std::string_view fragment = openTagFragment(std::string_view(text));
    if (!fragment.empty()) {
      std::string probe(text.substr(0, text.size() - fragment.size()));
      result.stats.probes++;
      result.bytesParsed += probe.size();
      ParseResult probed = parseMarkdownAt(probe, origin, parse);
      if (provesTagRepair(current, probed, fragment)) {
        result.stats.applied++;
        text = std::move(probe);
        current = std::move(probed);
      }
    }
  }

  // An unfinished formula before the spans, because its contents are verbatim:
  // a `*` or a `[` inside `$$x + ` is TeX and must not be styled by a later
  // repair. One probe, and only when the math feature is on.
  const size_t openRun =
      parse.enabled() && options_.has(PreviewFeature::Math) ? openMathRun(std::string_view(text), parse) : 0;
  if (openRun != 0) {
    // The closer goes at the very end of the region, not next to the opener: it
    // is the end of the last line that is the only place md4c can pair it from,
    // and the trailing whitespace that a chunk boundary often lands on belongs
    // inside the formula, where TeX ignores it. Its length is the opener's,
    // because md4c pairs a run only with a run of the same length.
    std::string probe = text;
    probe.append(openRun, '$');
    result.stats.probes++;
    result.bytesParsed += probe.size();
    ParseResult probed = parseMarkdownAt(probe, origin, parse);
    if (provesMathRepair(current, probed)) {
      result.stats.applied++;
      text = std::move(probe);
      current = std::move(probed);
      // A formula now exists, and its contents are verbatim, so a link or a
      // span repair after it would act on TeX. The pass is done.
      result.repaired = true;
      result.text = std::move(text);
      result.parsed = std::move(current);
      return result;
    }
  }

  // Closers for spans outside any repaired link go after the content; closers
  // for spans inside the link text go before its `]`. Until a link is repaired
  // there is no inside.
  size_t outerAt = trailing.start;
  size_t innerAt = std::string::npos;
  size_t linkTextStart = std::u16string::npos;

  // An unfinished link or image first, because its partial target is not
  // content: a span opened inside `https://exa_m` must not be styled, and
  // dropping the target removes it before the spans are looked for.
  if (options_.has(PreviewFeature::Links) || options_.has(PreviewFeature::Images)) {
    const auto link = findOpenLink(std::string_view(text).substr(0, trailing.start));
    if (link && options_.has(link->image ? PreviewFeature::Images : PreviewFeature::Links)) {
      std::string probe = text.substr(0, link->close);
      probe += "]()";
      probe.append(text, trailing.start, std::string::npos);
      result.stats.probes++;
      result.bytesParsed += probe.size();
      ParseResult probed = parseMarkdownAt(probe, origin, parse);
      size_t textStart = 0;
      if (provesLinkRepair(current, probed, link->image, textStart)) {
        result.stats.applied++;
        innerAt = link->close;
        outerAt = link->close + 3;
        linkTextStart = textStart;
        text = std::move(probe);
        current = std::move(probed);
      }
    }
  }

  // Innermost first. A candidate that fails its proof, such as a `*` the writer
  // escaped, is skipped rather than ending the pass, so it cannot hide a real
  // opener before it. Each proven closer becomes the baseline for the next, so
  // `~~strike **bold` closes both spans, and a candidate before the one just
  // closed keeps its offset because only text after it moved.
  uint64_t spanProbes = 0;
  for (const OpenSpan &span : findOpenSpans(current.document->blocks.back())) {
    if (spanProbes >= kMaxProbes) {
      break;
    }
    const bool isCode = span.marker == u'`';
    if (isCode ? !options_.has(PreviewFeature::InlineCode) : !options_.has(PreviewFeature::Emphasis)) {
      continue;
    }
    const bool inside = linkTextStart != std::u16string::npos && span.start >= linkTextStart;
    std::string probe(text);
    probe.insert(inside ? innerAt : outerAt, span.length, static_cast<char>(span.marker));
    spanProbes++;
    result.stats.probes++;
    result.bytesParsed += probe.size();
    ParseResult probed = parseMarkdownAt(probe, origin, parse);
    if (!provesRepair(current, probed, span)) {
      continue;
    }
    result.stats.applied++;
    if (inside) {
      innerAt += span.length;
    }
    outerAt += span.length;
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
