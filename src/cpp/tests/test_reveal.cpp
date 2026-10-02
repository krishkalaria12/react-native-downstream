// Engine checks for the streaming reveal: what counts as new, where the unit
// boundaries fall, when each unit starts, how a unit's alpha is read at a
// moment, and where a caret goes.
//
// These are the rules both platforms draw with, so they are checked here against
// the real parser rather than in a view that only one platform can run.
//
//     node --test src/cpp/tests/engine.test.mjs   (runs test_reveal)

#include "Caret.h"
#include "MarkdownParser.h"
#include "RenderPreparer.h"
#include "Reveal.h"
#include "Scan.h"
#include "tests/check.h"
#include "Unicode.h"

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace downstream;

namespace {

std::u16string utf16(std::string_view text) { return utf8ToUtf16(text); }

/// The offsets of the ranges the alignment says were already on screen.
std::vector<uint32_t> shown(const std::vector<AlignedRun> &runs, uint32_t length) {
  std::vector<uint32_t> offsets;
  uint32_t cursor = 0;
  for (const AlignedRun &run : runs) {
    for (uint32_t at = run.nextStart; at < run.nextStart + run.length; at++) {
      offsets.push_back(at);
    }
    cursor = std::max(cursor, run.nextStart + run.length);
  }
  (void)length;
  return offsets;
}

bool checkOffsets(std::string_view previous, std::string_view next, std::vector<uint32_t> expected, const char *label) {
  const auto actual = shown(alignVisibleText(utf16(previous), utf16(next)), static_cast<uint32_t>(next.size()));
  if (actual.size() != expected.size()) {
    std::fprintf(stderr, "FAIL %s: expected %zu offsets, actual %zu\n", label, expected.size(), actual.size());
    return false;
  }
  for (size_t index = 0; index < actual.size(); index++) {
    if (actual[index] != expected[index]) {
      std::fprintf(stderr, "FAIL %s: offset %zu is %u, expected %u\n", label, index, actual[index], expected[index]);
      return false;
    }
  }
  return true;
}

/// The unit ranges of one block's whole text, as `[start, end]` pairs in
/// UTF-16 offsets.
std::vector<std::pair<uint32_t, uint32_t>> units(std::u16string_view text, RevealSplit split,
                                                 BlockKind kind = BlockKind::Paragraph) {
  Block source;
  source.text = std::u16string(text);
  source.kind = kind;
  std::vector<RevealUnit> cut;
  splitUnits(source, 0, 0, static_cast<uint32_t>(source.text.size()), split, cut);
  std::vector<std::pair<uint32_t, uint32_t>> ranges;
  for (const RevealUnit &unit : cut) {
    ranges.emplace_back(unit.start, unit.end);
  }
  return ranges;
}

bool checkUnits(std::u16string_view text, RevealSplit split, std::vector<std::pair<uint32_t, uint32_t>> expected,
                const char *label) {
  const auto actual = units(text, split);
  if (actual.size() != expected.size()) {
    std::fprintf(stderr, "FAIL %s: expected %zu units, actual %zu\n", label, expected.size(), actual.size());
    return false;
  }
  for (size_t index = 0; index < actual.size(); index++) {
    if (actual[index] != expected[index]) {
      std::fprintf(stderr, "FAIL %s: unit %zu is %u-%u, expected %u-%u\n", label, index, actual[index].first,
                   actual[index].second, expected[index].first, expected[index].second);
      return false;
    }
  }
  return true;
}

// ------------------------------------------------------------------ alignment

void testAlignment() {
  CHECK(checkOffsets("hello **wor", "hello world", {0, 1, 2, 3, 4, 5, 6, 7, 8},
                     "a bold word closing fades only what the markers hid"),
        "a bold word closing fades the tail it revealed");
  CHECK(checkOffsets("hello world", "hello world", {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10},
                     "an unchanged block is entirely old"),
        "an unchanged block has nothing new");
  CHECK(checkOffsets("[docs](https://exa", "docs and more", {0, 1, 2, 3},
                     "an unfinished link keeps the label it already showed"),
        "an unfinished link's markers are skipped and its label stays old");
  CHECK(checkOffsets("[docs](https://exa", "docs", {0, 1, 2, 3}, "a link that closes keeps its label"),
        "a closing link keeps its label old");
  CHECK(checkOffsets("~~strike", "strike", {0, 1, 2, 3, 4, 5}, "strikethrough markers are skipped"),
        "a closing strike skips its markers");
  CHECK(checkOffsets("`code", "code", {0, 1, 2, 3}, "a closing backtick is skipped"),
        "a closing code span skips its backtick");
  CHECK(checkOffsets("Title\n===", "Title", {0, 1, 2, 3, 4}, "a setext heading keeps its text"),
        "setext underline changes produce nothing new");
  CHECK(checkOffsets("a\nb", "a\nb\nc", {0, 1, 2}, "an appended character is new"),
        "an appended character after a hard break is new");
  CHECK(checkOffsets("", "hello", {}, "an empty previous block has nothing old"),
        "a first word in an empty block is entirely new");

  // A very long block falls back to the count rule rather than walking it.
  const std::u16string longPrevious(70000, u'a');
  const std::u16string longNext(70000, u'a');
  CHECK(alignVisibleText(longPrevious, longNext).size() == 1, "a huge unchanged block is one run");
  const auto byCount = alignVisibleText(longPrevious, std::u16string(69999, u'a') + u'b');
  CHECK(byCount.size() == 1 && byCount[0].length == 70000, "a huge block uses the count rule");

  // Alignment is monotonic: the runs cover `next` in order and never overlap.
  const std::u16string messyPrev = utf16("**bold** and `code` and [a](b) tail");
  const std::u16string messyNext = utf16("bold and code and a tail plus more");
  uint32_t cursor = 0;
  bool ordered = true;
  for (const AlignedRun &run : alignVisibleText(messyPrev, messyNext)) {
    ordered = ordered && run.nextStart >= cursor;
    cursor = run.nextStart + run.length;
  }
  CHECK(ordered, "aligned runs are in order and do not overlap");
}

// ---------------------------------------------------------------------- units

void testUnits() {
  CHECK(checkUnits(u"hello world", RevealSplit::Word, {{0, 6}, {6, 11}}, "a word takes its trailing space"),
        "two words are two units");
  CHECK(checkUnits(u"hello world", RevealSplit::Char,
                   {{0, 1}, {1, 2}, {2, 3}, {3, 4}, {4, 6}, {6, 7}, {7, 8}, {8, 9}, {9, 10}, {10, 11}},
                   "char mode is one cluster at a time, space on the letter before"),
        "char mode gives each character its own unit and glues the space");
  CHECK(checkUnits(u"a  b", RevealSplit::Word, {{0, 3}, {3, 4}}, "a run of spaces joins the word before it"),
        "a run of spaces joins the word before it");
  CHECK(checkUnits(u"a\nb", RevealSplit::Word, {{0, 1}, {2, 3}}, "a line break ends a unit"),
        "a line break is not glued to the word");
  CHECK(checkUnits(u"\x4E2D\x6587", RevealSplit::Word, {{0, 1}, {1, 2}}, "a CJK run is one unit per character"),
        "ideographs are one unit each");
  CHECK(checkUnits(u"\xD55C\xAD6D\xC5B4", RevealSplit::Word, {{0, 1}, {1, 2}, {2, 3}}, "Hangul is one unit each"),
        "Hangul syllables are one unit each");
  // A four-unit emoji is a surrogate pair plus two joiners, and char mode has to
  // keep it in one piece or half the glyph appears at a different alpha.
  CHECK(checkUnits(u"\xD83D\xDC69\x200D\xD83C\xDFA2", RevealSplit::Char, {{0, 5}},
                   "a ZWJ emoji is one cluster"),
        "a zero-width-joiner sequence stays whole");
  CHECK(checkUnits(u"\xD83C\xDDE6\xD83C\xDDFA", RevealSplit::Char, {{0, 4}}, "a flag is one cluster"),
        "a regional-indicator pair stays whole");
  CHECK(checkUnits(u"e\u0301cole", RevealSplit::Char, {{0, 2}, {2, 3}, {3, 4}, {4, 5}, {5, 6}},
                   "a combining mark stays with its base"),
        "a combining mark stays with its base");
  CHECK(checkUnits(u"a\U0001F600b", RevealSplit::Char, {{0, 1}, {1, 3}, {3, 4}}, "a surrogate pair stays whole"),
        "an emoji is one cluster");
  CHECK(checkUnits(u"", RevealSplit::Word, {{0, 1}}, "an empty block is one unit"), "an empty block still fades");
  CHECK(checkUnits(u"", RevealSplit::Word, {{0, 1}}, "an empty block is one unit in char mode too") ||
            checkUnits(u"", RevealSplit::Char, {{0, 1}}, "an empty block is one unit in char mode too"),
        "an empty block is one unit whatever the split");

  Block row;
  row.kind = BlockKind::TableRow;
  row.text = utf16("a\tb\tc");
  std::vector<RevealUnit> cut;
  splitUnits(row, 3, 0, static_cast<uint32_t>(row.text.size()), RevealSplit::Char, cut);
  CHECK(cut.size() == 1 && cut[0].start == 0 && cut[0].end == 5 && cut[0].block == 3,
        "a table row is one unit even in char mode");

  Block rule;
  rule.kind = BlockKind::ThematicBreak;
  std::vector<RevealUnit> ruleUnits;
  splitUnits(rule, 0, 0, 0, RevealSplit::Word, ruleUnits);
  CHECK(ruleUnits.size() == 1, "a rule is one unit");
}

// ------------------------------------------------------------------- timeline

void testTimeline() {
  RevealOptions options;

  // One pass, four words, 40 ms apart, starting now.
  {
    RevealTimeline timeline;
    std::vector<RevealUnit> batch(4);
    timeline.schedule(batch, 1000, options);
    CHECK(batch[0].startAt == 1000, "the first unit starts now");
    CHECK(batch[3].startAt == 1120, "units are spaced by the stagger");
  }
  // The cursor carries across passes, which is what serializes blocks.
  {
    RevealTimeline timeline;
    std::vector<RevealUnit> first(4);
    timeline.schedule(first, 1000, options);
    std::vector<RevealUnit> second(2);
    timeline.schedule(second, 1000, options);
    CHECK(second[0].startAt == 1160, "a later batch starts after the first drains");
  }
  // A gap in the stream releases the debt rather than keeping it.
  {
    RevealTimeline timeline;
    std::vector<RevealUnit> first(4);
    timeline.schedule(first, 1000, options);
    std::vector<RevealUnit> second(2);
    timeline.schedule(second, 2000, options);
    CHECK(second[0].startAt == 2000, "an idle gap resets the cursor to now");
  }
  // A burst compresses into the budget instead of queueing seconds of words.
  {
    RevealTimeline timeline;
    std::vector<RevealUnit> burst(40);
    timeline.schedule(burst, 0, options);
    CHECK(burst.back().startAt <= kRevealMaxBacklogMs, "a burst finishes inside the backlog");
    CHECK(burst.back().startAt > 0, "a compressed burst still cascades");
    for (size_t index = 1; index < burst.size(); index++) {
      CHECK(burst[index].startAt >= burst[index - 1].startAt, "compression keeps the order");
    }
  }
  // Once the budget is spent the floor holds, so words do not all fire at once.
  {
    RevealTimeline timeline;
    std::vector<RevealUnit> first(40);
    timeline.schedule(first, 0, options);
    std::vector<RevealUnit> second(40);
    timeline.schedule(second, 0, options);
    const double step = second[1].startAt - second[0].startAt;
    CHECK(step >= kRevealMinStaggerStepMs - 1e-9, "an exhausted budget keeps a minimum cascade");
  }
  // `stagger: 0` means every unit at once, with no floor.
  {
    options.staggerMs = 0;
    RevealTimeline timeline;
    std::vector<RevealUnit> burst(40);
    timeline.schedule(burst, 500, options);
    CHECK(burst.back().startAt == 500, "a zero stagger starts every unit together");
    options.staggerMs = 40;
  }
}

// -------------------------------------------------------------------- easing

void testEasing() {
  // Reference values for the CSS keywords, to three decimals.
  const CubicBezier ease(0.25f, 0.1f, 0.25f, 1.0f);
  CHECK(std::fabs(ease.solve(0.0f) - 0.0f) < 1e-4, "ease starts at zero");
  CHECK(std::fabs(ease.solve(1.0f) - 1.0f) < 1e-4, "ease ends at one");
  CHECK(std::fabs(ease.solve(0.5f) - 0.8024f) < 1e-3, "ease at the midpoint matches the CSS curve");

  const CubicBezier linear(0.0f, 0.0f, 1.0f, 1.0f);
  CHECK(std::fabs(linear.solve(0.25f) - 0.25f) < 1e-4, "linear is the identity");
  CHECK(std::fabs(linear.solve(0.75f) - 0.75f) < 1e-4, "linear is the identity at three quarters");

  const CubicBezier easeIn(0.42f, 0.0f, 1.0f, 1.0f);
  const CubicBezier easeOut(0.0f, 0.0f, 0.58f, 1.0f);
  const CubicBezier easeInOut(0.42f, 0.0f, 0.58f, 1.0f);
  CHECK(easeIn.solve(0.5f) < 0.5f, "ease-in is slower at the midpoint");
  CHECK(easeOut.solve(0.5f) > 0.5f, "ease-out is faster at the midpoint");
  CHECK(std::fabs(easeInOut.solve(0.5f) - 0.5f) < 1e-3, "ease-in-out is symmetric at the midpoint");
  CHECK(std::fabs(easeInOut.solve(0.25f) - 0.1296f) < 1e-3, "ease-in-out at a quarter matches the CSS curve");
}

// --------------------------------------------------------------------- alpha

void testAlpha() {
  RevealOptions options;
  options.effect = RevealEffect::Fade;
  options.durationMs = 150;
  RevealUnit unit{0, 0, 5, 1000};
  CHECK(revealAlpha(unit, 999, options) == 0.0f, "a unit is invisible before it starts");
  CHECK(revealAlpha(unit, 1000, options) == 0.0f, "a unit is invisible at its start");
  CHECK(revealAlpha(unit, 1001, options) > 0.0f, "a unit has begun to appear one millisecond in");
  CHECK(revealAlpha(unit, 1075, options) > 0.0f && revealAlpha(unit, 1075, options) < 1.0f,
        "a unit is still fading halfway through");
  CHECK(revealAlpha(unit, 1150, options) == 1.0f, "a unit is opaque after its duration");
  CHECK(revealAlpha(unit, 9999, options) == 1.0f, "a unit stays opaque");

  options.durationMs = 0;
  CHECK(revealAlpha(unit, 1000, options) == 1.0f, "a zero duration shows text at once");

  options.durationMs = 150;
  options.effect = RevealEffect::None;
  CHECK(revealAlpha(unit, 0, options) == 1.0f, "no fade means every character is opaque");
  options.effect = RevealEffect::Rise;
  CHECK(revealRise(unit, 1000, options) == kRevealRisePoints, "a rise starts at its full offset");
  CHECK(revealRise(unit, 1075, options) > 0 && revealRise(unit, 1075, options) < kRevealRisePoints,
        "a rise is halfway at the midpoint");
  CHECK(revealRise(unit, 1150, options) == 0.0f, "a rise has landed at the end");
}

// ----------------------------------------------------------------- the fence

void testFenceOpen() {
  CHECK(scan::endsInsideFence("```ts\nconst a = 1;\n"), "an unclosed fence is still open");
  CHECK(!scan::endsInsideFence("```ts\nconst a = 1;\n```\n"), "a closed fence is not open");
  CHECK(!scan::endsInsideFence("no fence here\n"), "prose is not a fence");
  CHECK(!scan::endsInsideFence("~~~\ncode\n~~~~\n"), "a longer closer closes a shorter opener");
  CHECK(scan::endsInsideFence("~~~\ncode\n"), "a tilde fence can be open too");
  CHECK(scan::endsInsideFence("````\n```\n"), "a nested shorter fence is content");
  CHECK(!scan::endsInsideFence("````\n```\n````\n"), "the long fence closes on its own closer");
  CHECK(!scan::endsInsideFence("    indented code\n"), "indented code is not a fence");

  const ParseResult open = parseMarkdown("```ts\nx\n");
  CHECK(open.document->blocks.size() == 1 && open.document->blocks[0].fenceOpen,
        "the parser marks an open fence");
  const ParseResult closed = parseMarkdown("```ts\nx\n```\n");
  CHECK(!closed.document->blocks[0].fenceOpen, "the parser leaves a closed fence alone");
  const ParseResult indented = parseMarkdown("    x\n");
  CHECK(!indented.document->blocks[0].fenceOpen, "indented code is never marked open");
  const ParseResult twoBlocks = parseMarkdown("para\n\n```ts\nx\n");
  CHECK(twoBlocks.document->blocks.size() == 2 && twoBlocks.document->blocks[1].fenceOpen,
        "the flag lands on the last block");
}

// ----------------------------------------------------------------- the caret

CaretSpec spec(CaretKind kind, Animating animating) {
  CaretSpec result;
  result.kind = kind;
  result.animating = animating;
  return result;
}

CaretPlace place(std::string_view source, CaretSpec with) {
  const ParseResult parsed = parseMarkdown(source);
  return placeCaret(*parsed.document, with);
}

void testCaret() {
  CHECK(place("hello", spec(CaretKind::None, Animating::On)) == CaretPlace::None, "no shape means no caret");
  CHECK(place("hello", spec(CaretKind::Block, Animating::Off)) == CaretPlace::None,
        "a controlled view has to say the text is arriving");
  CHECK(place("hello", spec(CaretKind::Block, Animating::On)) == CaretPlace::AfterText, "a caret follows prose");

  // `auto` follows the document's own flag, which is what a stream sets.
  {
    Document streaming;
    streaming.streaming = true;
    const ParseResult parsed = parseMarkdown("hello");
    CHECK(placeCaret(*parsed.document, spec(CaretKind::Circle, Animating::Off)) == CaretPlace::None,
          "off wins over a streaming document");
    auto copy = std::make_shared<Document>(*parsed.document);
    copy->streaming = true;
    CHECK(placeCaret(*copy, spec(CaretKind::Circle, Animating::Auto)) == CaretPlace::AfterText,
          "auto follows a streaming document");
    CHECK(placeCaret(*copy, spec(CaretKind::Circle, Animating::Off)) == CaretPlace::None,
          "off stays off on a streaming document");
  }

  const Document empty;
  CHECK(placeCaret(empty, spec(CaretKind::Block, Animating::On)) == CaretPlace::OwnLine,
        "an empty document gives the caret a line");
  CHECK(place("# heading\n", spec(CaretKind::Block, Animating::On)) == CaretPlace::AfterText,
        "a caret follows a heading");
  CHECK(place("- item\n", spec(CaretKind::Block, Animating::On)) == CaretPlace::AfterText,
        "a caret follows a list item");
  CHECK(place("```ts\nx\n", spec(CaretKind::Block, Animating::On)) == CaretPlace::AfterText,
        "a caret sits inside an open fence");
  CHECK(place("```ts\nx\n```\n", spec(CaretKind::Block, Animating::On)) == CaretPlace::OwnLine,
        "a closed fence gives the caret its own line");
  CHECK(place("| a |\n| - |\n| b |\n", spec(CaretKind::Block, Animating::On)) == CaretPlace::OwnLine,
        "a table gives the caret its own line");
  CHECK(place("---\n", spec(CaretKind::Block, Animating::On)) == CaretPlace::OwnLine,
        "a rule gives the caret its own line");
  CHECK(place("- ```ts\n  x\n", spec(CaretKind::Block, Animating::On)) == CaretPlace::AfterText,
        "an open fence inside a list still counts as open");

  // A layout hint that differs only in the caret is a different layout, because
  // the placeholder is in the measured text.
  LayoutHint plain(1, 320);
  LayoutHint withCaret(1, 320, kChromeNone, CodeLayout{}, PlacedCaret{CaretKind::Block, CaretPlace::AfterText});
  CHECK(!(plain == withCaret), "the caret is part of a layout hint");
  LayoutHint ownLine(1, 320, kChromeNone, CodeLayout{}, PlacedCaret{CaretKind::Block, CaretPlace::OwnLine});
  CHECK(!(withCaret == ownLine), "an own-line caret is a different layout");
  PlacedCaret resolved;
  resolved.kind = CaretKind::None;
  CHECK(resolveCaret(Document{}, CaretSpec{}) == resolved, "a document with no caret shape resolves to none");
}

} // namespace

int main() {
  testAlignment();
  testUnits();
  testTimeline();
  testEasing();
  testAlpha();
  testFenceOpen();
  testCaret();
  return ds_check_result();
}