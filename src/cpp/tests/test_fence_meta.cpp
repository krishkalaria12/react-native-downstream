// Engine checks for the fence info string: the meta grammar, the prefix
// property that makes it safe while a fence streams, and the identity rule that
// a meta-only change must break.
//
//     node --test src/cpp/tests/engine.test.mjs   (runs test_fence_meta)

#include "Compare.h"
#include "FenceMeta.h"
#include "MarkdownParser.h"
#include "tests/check.h"

#include <string>
#include <string_view>
#include <vector>

using namespace downstream;

namespace {

void checkTitle(const char *expected, const char *label, std::string_view info) {
  // Held in a named variable: the macro compares the pointer, so a temporary
  // would be gone before it is read.
  const std::string actual = parseFenceMeta(info).title;
  CHECK_EQ_STR(actual.c_str(), expected, label);
}

void checkStartLine(uint32_t expected, const char *label, std::string_view info) {
  const uint32_t actual = parseFenceMeta(info).startLine;
  if (actual != expected) {
    std::fprintf(stderr, "FAIL %s: expected %u, actual %u\n", label, expected, actual);
    ds_check_failures++;
  }
}

void checkNoNumbers(bool expected, const char *label, std::string_view info) {
  const bool actual = parseFenceMeta(info).noLineNumbers;
  if (actual != expected) {
    std::fprintf(stderr, "FAIL %s: expected %s\n", label, expected ? "hidden" : "shown");
    ds_check_failures++;
  }
}

void checkMarks(const std::vector<LineMark> &expected, const char *label, std::string_view info) {
  const std::vector<LineMark> actual = parseFenceMeta(info).marks;
  if (actual.size() != expected.size()) {
    std::fprintf(stderr, "FAIL %s: expected %zu marks, actual %zu\n", label, expected.size(), actual.size());
    ds_check_failures++;
    return;
  }
  for (size_t index = 0; index < actual.size(); index++) {
    if (actual[index] != expected[index]) {
      std::fprintf(stderr, "FAIL %s: mark %zu is %u-%u, expected %u-%u\n", label, index, actual[index].first,
                   actual[index].last, expected[index].first, expected[index].last);
      ds_check_failures++;
      return;
    }
  }
}

// ------------------------------------------------------------ the grammar

void testTitles() {
  // Streamdown names a download from the language alone, so it has no title.
  checkTitle("", "no title is no title", "ts");
  // Docusaurus, rehype-pretty code, and Expressive Code spell it `title`.
  checkTitle("app.ts", "a double-quoted title", "ts title=\"app.ts\"");
  checkTitle("app.ts", "a single-quoted title", "ts title='app.ts'");
  checkTitle("app", "a bare title", "ts title=app");
  // Nextra spells it `filename`.
  checkTitle("app.ts", "filename is a title too", "ts filename=\"app.ts\"");
  // #16's rule: `lang:path` names the file, kept as the fallback.
  checkTitle("src/app.ts", "a lang:path is a title", "ts:src/app.ts");
  // An explicit title beats the path, whichever came first.
  checkTitle("a.ts", "title beats a later path", "ts:src/app.ts title=\"a.ts\"");
  // A fence may write its name once, as `ts title="a.ts":src/app.ts`, where the
  // path is decoration after the quoted name rather than a second name.
  checkTitle("a.ts", "a path after a quoted title is part of that title's token", "ts title=\"a.ts\":x");
  // A title with spaces stays one token, in either quote.
  checkTitle("my file.ts", "a quoted title holds spaces", "ts title=\"my file.ts\"");
  // A bare value ends at whitespace, as #16's own scan did.
  checkTitle("app", "a bare title ends at whitespace", "ts title=app other=1");

  // The first one wins, so a later token cannot replace a name the writer put
  // first.
  checkTitle("a.ts", "the first title wins", "ts title=\"a.ts\" title=\"b.ts\"");
  checkTitle("a.ts", "title wins over filename", "ts title=\"a.ts\" filename=\"b.ts\"");

  // Streaming safety: a title that has not closed yet is not a title. The label
  // shows the language until the closing quote arrives.
  checkTitle("", "an unterminated quote is no title", "ts title=\"src/ap");
  checkTitle("", "an unterminated single quote is no title", "ts title='src/ap");
  checkTitle("src/ap", "the closing quote completes it", "ts title=\"src/ap\"");

  // A model can write anything, so the units that could repoint a name at
  // something else are removed. `exe.txt` must not read as `txt.exe`.
  // The bytes are U+202E and a tab, written out because a source literal is not
  // portable.
  const std::string rtl = "ts title=\"a\xe2\x80\xae.txt\"";
  checkTitle("a.txt", "a bidi override is removed", rtl);
  checkTitle("ab", "a tab inside a title is removed", "ts title=\"a\tb\"");
  checkTitle("ab", "a newline inside a title is removed", "ts title=\"a\nb\"");
  checkTitle("ab", "a DEL is removed", "ts title=\"a\x7f""b\"");

  // A title is capped at 96 UTF-16 units, so one header row can hold it.
  const std::string longTitle = "ts title=\"" + std::string(400, 'a') + "\"";
  CHECK(parseFenceMeta(longTitle).title.size() == kMaxTitleUnits, "a long title is capped");
  // An astral character is two UTF-16 units, so the cap never splits one: 48 of
  // them is exactly 96 units, and the 49th would take the title past the cap.
  std::string wide = "ts title=\"";
  for (int repeat = 0; repeat < 100; repeat++) {
    wide.append("\xf0\x9f\x91\xa9");
  }
  wide.push_back('"');
  // 48 astral characters are 96 UTF-16 units and 192 UTF-8 bytes, so the byte
  // length is twice the unit count: the cap is counting units, not bytes.
  CHECK(parseFenceMeta(wide).title.size() == 2 * kMaxTitleUnits, "the cap counts UTF-16 units");
  // A title of nothing but bidi controls is no title, rather than an empty label
  // beside the language. The bytes are U+202E U+202E, written out because a
  // source literal is not portable.
  checkTitle("", "a title of only controls is no title", "ts title=\"\xe2\x80\xae\xe2\x80\xae\"");

  // The parse is bounded, though the raw string is kept whole in Block::info.
  const std::string huge = "ts title=\"a\" " + std::string(4096, 'x');
  checkTitle("a", "only the first kibibyte is parsed", huge);
}

void testStartLines() {
  // Streamdown's spelling.
  checkStartLine(10, "startLine=N", "ts startLine=10");
  checkStartLine(1, "startLine=1", "ts startLine=1");
  checkStartLine(1000000, "the largest start line", "ts startLine=1000000");
  // Docusaurus, rehype-pretty code, and Nextra spell it these ways.
  checkStartLine(5, "showLineNumbers=N", "ts showLineNumbers=5");
  checkStartLine(5, "showLineNumbers{N}", "ts showLineNumbers{5}");
  // `startLine` wins whichever order the two arrive in.
  checkStartLine(9, "startLine beside showLineNumbers", "ts startLine=9 showLineNumbers=5");
  checkStartLine(9, "showLineNumbers beside startLine", "ts showLineNumbers=5 startLine=9");

  // Anything below 1 or above the bound is ignored, so the fence numbers from
  // one rather than drawing a gutter nothing can hold.
  checkStartLine(1, "zero is ignored", "ts startLine=0");
  checkStartLine(1, "a negative is ignored", "ts startLine=-3");
  checkStartLine(1, "trailing digits are not a number", "ts startLine=10abc");
  checkStartLine(1, "past the bound is ignored", "ts startLine=1000001");
  checkStartLine(1, "a very long value is ignored", "ts startLine=99999999999");
  checkStartLine(1, "an empty value is ignored", "ts startLine=");
  checkStartLine(1, "no value at all is ignored", "ts startLine");
  checkStartLine(1, "hex is not a line number", "ts startLine=0x10");

  // A bare `showLineNumbers` says nothing: numbers are already on by default.
  checkStartLine(1, "a bare showLineNumbers changes no line", "ts showLineNumbers");
}

void testLineNumberFlags() {
  // Exact and case-sensitive, so a longer token that contains this one does not
  // turn numbers off.
  checkNoNumbers(true, "noLineNumbers", "ts noLineNumbers");
  checkNoNumbers(false, "xnoLineNumbers is not the key", "ts xnoLineNumbers");
  checkNoNumbers(false, "noLineNumbersX is not the key", "ts noLineNumbersX");
  checkNoNumbers(false, "the key is case-sensitive", "ts nolineNumbers");
  // It rides with other tokens rather than replacing them.
  checkNoNumbers(true, "noLineNumbers beside a title", "ts title=\"a.ts\" noLineNumbers");
  // `showLineNumbers` on its own does not hide anything.
  checkNoNumbers(false, "showLineNumbers is not a hiding key", "ts showLineNumbers");
}

void testLineMarks() {
  checkMarks({{1, 1}}, "one line", "ts {1}");
  checkMarks({{1, 1}, {3, 5}}, "a list and a range", "ts {1,3-5}");
  // Sorted and merged, so two spellings of one set compare equal.
  checkMarks({{1, 3}, {5, 5}}, "an unsorted list is sorted", "ts {5,1-3}");
  checkMarks({{1, 5}}, "overlapping ranges merge", "ts {1-3,2-5}");
  checkMarks({{1, 5}}, "touching ranges merge", "ts {1-3,4-5}");
  checkMarks({{1, 1}, {3, 3}}, "a gap stays a gap", "ts {1,3}");
  // Unusable entries contribute nothing rather than a range to guess at.
  checkMarks({}, "an empty list is no marks", "ts {}");
  checkMarks({}, "a reversed range is ignored", "ts {5-1}");
  checkMarks({}, "a zero line is ignored", "ts {0}");
  checkMarks({{2, 2}}, "a good entry survives a bad one", "ts {x,2}");
  checkMarks({{1, 1000000}}, "the largest line is kept", "ts {1-1000000}");
  // At most 64, because a chat model rarely writes any at all.
  {
    std::string many = "ts {";
    for (int index = 1; index <= 200; index++) {
      many += std::to_string(index * 2);
      many += ",";
    }
    many.pop_back();
    many += "}";
    CHECK(parseFenceMeta(many).marks.size() <= kMaxLineMarks, "marks are capped");
  }
  // An unterminated brace drops the token rather than guessing where it ended.
  checkMarks({}, "an unterminated brace is no marks", "ts {1,3");
  // A mark never leaks into the title: `{}` and `{1}` are consumed whole.
  checkTitle("", "a mark list is not a title", "ts {1,3}");
}

// -------------------------------------------------- the streaming property

/// Whether `prefix` ends inside an unterminated quote, which is what makes a
/// half-typed quoted title yield nothing rather than something wrong.
///
/// A half-typed brace is a different case: it drops the marks after it and
/// leaves an already-closed title standing, because a completed token before an
/// unfinished one is finished.
bool hasUnterminatedQuote(std::string_view prefix) {
  bool inQuote = false;
  char quote = 0;
  for (const char c : prefix) {
    if (inQuote) {
      inQuote = c != quote;
    } else if (c == '"' || c == '\'') {
      inQuote = true;
      quote = c;
    }
  }
  return inQuote;
}

/**
 * Every prefix of an info string parses without failing, grows its title toward
 * the whole's rather than inventing one, and yields the same answer whether or
 * not its prefixes were parsed first.
 *
 * This is the meta's version of the streaming equivalence test. A fence whose
 * info string is still being typed shows the language until its title closes,
 * and a half-typed title shows its own prefix rather than a wrong one.
 */
void testPrefixes() {
  const std::vector<std::string> fixtures = {
      "ts",
      "ts title=\"app.ts\"",
      "ts title='src/app.ts'",
      "ts startLine=10",
      "ts startLine=10 noLineNumbers",
      "ts title=\"a b\" startLine=9 {1,3-5}",
      "ts:src/app.ts",
      "ts filename=\"x\"",
      "ts showLineNumbers{5}",
      "ts title=\"src/ap",
      "ts title=\"startLine=9\"",
      "ts wrap ins={1} del={2}",
      "ts {1,3",
      "ts title=bare title=bare2",
      "ts showLineNumbers=5 startLine=9",
      // A title with a zero-width space, which the sanitizer removes. The bytes
      // are U+200B, written out because a source literal is not portable.
      "ts title=\"a\xe2\x80\x8b" "b\" noLineNumbers",
  };
  for (const std::string &fixture : fixtures) {
    const FenceMeta whole = parseFenceMeta(fixture);
    for (size_t length = 0; length <= fixture.size(); length++) {
      const std::string_view prefix(fixture.data(), length);
      const FenceMeta partial = parseFenceMeta(prefix);
      // Every prefix is a valid parse: the start line is in range, marks are
      // sorted, and the title is sanitized and capped.
      CHECK(partial.startLine >= 1 && partial.startLine <= kMaxStartLine, "a prefix's start line is in range");
      CHECK(partial.title.size() <= kMaxTitleUnits, "a prefix's title is capped");
      for (size_t index = 1; index < partial.marks.size(); index++) {
        CHECK(partial.marks[index - 1].last < partial.marks[index].first, "a prefix's marks are sorted");
      }
      // A title from an unterminated quote would be a wrong title rather than a
      // partial one, so a half-typed run yields nothing at all.
      if (hasUnterminatedQuote(prefix)) {
        CHECK(partial.title.empty(), "an unclosed title yields no title");
      }
      // And no prefix ever shows something the whole string does not begin with,
      // so a title a reader saw while streaming is a prefix of the final one.
      if (!partial.title.empty()) {
        CHECK(whole.title.compare(0, partial.title.size(), partial.title) == 0,
              "a prefix's title begins the whole's title");
      }
      // A prefix never names a line past the whole's: the whole parsed every
      // byte the prefix has, so a later token can only override downwards.
      if (whole.startLine != 1) {
        CHECK(partial.startLine <= whole.startLine, "a prefix's start line is one or the whole's");
      }
      // Parsing the same prefix again, and parsing it after another string,
      // gives the same answer. Nothing is carried between calls.
      CHECK(parseFenceMeta(prefix) == partial, "a prefix parses the same twice");
      (void)parseFenceMeta("ts title=\"other\" startLine=3");
      CHECK(parseFenceMeta(prefix) == partial, "a prefix parses the same after another");
    }
    // The whole string parses the same whether or not its prefixes were parsed
    // first, which is what a streaming tail does on every snapshot.
    CHECK(parseFenceMeta(fixture) == whole, "the whole string is unaffected by its prefixes");
  }
}

// --------------------------------------------------------------- identity

void testIdentity() {
  ParseResult first = parseMarkdown("```ts title=\"a.ts\"\nx\n```\n");
  ParseResult second = parseMarkdown("```ts title=\"b.ts\"\nx\n```\n");
  CHECK(first.status == ParseStatus::Ok && second.status == ParseStatus::Ok, "both fences parse");
  // An identity promises that it names the same content. A block whose title
  // changed draws a different header label, so it is different content, and the
  // committed-segment cache keyed by identity would otherwise keep drawing the
  // old one.
  CHECK(!scan::sameBlock(first.document->blocks[0], second.document->blocks[0]),
        "a changed title gives a different block");
  CHECK(first.document->blocks[0].meta.title == "a.ts", "the first fence's title parses");
  CHECK(second.document->blocks[0].meta.title == "b.ts", "the second fence's title parses");

  // Only the meta changed: the text and the language are identical.
  ParseResult plain = parseMarkdown("```ts\nx\n```\n");
  CHECK(plain.document->blocks[0].text == first.document->blocks[0].text, "the code text is unchanged");
  CHECK(plain.document->blocks[0].language == first.document->blocks[0].language, "the language is unchanged");

  // A fence with no meta is an empty one, not a default one.
  CHECK(plain.document->blocks[0].meta == FenceMeta{}, "a plain fence has an empty meta");
  CHECK(plain.document->blocks[0].meta.startLine == 1, "an empty meta starts at line one");
  // The raw info string survives for a custom renderer, as Streamdown's
  // `CustomRendererProps.meta` does.
  CHECK(first.document->blocks[0].info == "ts title=\"a.ts\"", "the raw info string is kept");
}

/// md4c decodes an info string with a backslash escape or an entity into a
/// buffer of its own, so the decoded text says nothing about where the opener
/// sits in the source. The fence's own run still has to be read off the
/// opener line, or a shorter run inside the block would close it.
void testEscapedInfo() {
  const char *infos[] = {"j&amp;s", "j\\*s", "mer&#x61;id"};
  for (const char *info : infos) {
    const std::string open = std::string("Intro paragraph.\n\n````") + info + "\ncode\n```\nstill code\n";
    ParseResult parsed = parseMarkdown(open);
    CHECK(parsed.status == ParseStatus::Ok && parsed.document->blocks.size() == 2, "an escaped fence parses");
    const Block &block = parsed.document->blocks[1];
    CHECK(block.fenceMarker == '`' && block.fenceLength == 4, "an escaped info string keeps the fence's length");
    CHECK(!block.fenceClosed, "a shorter run inside the block does not close it");

    ParseResult closed = parseMarkdown(open + "````\n\nAfter.\n");
    CHECK(closed.status == ParseStatus::Ok && closed.document->blocks.size() == 3, "a closed escaped fence parses");
    CHECK(closed.document->blocks[1].fenceClosed, "a run as long as the opener closes it");
  }
}

} // namespace

int main() {
  testTitles();
  testStartLines();
  testLineNumberFlags();
  testLineMarks();
  testPrefixes();
  testIdentity();
  testEscapedInfo();
  return ds_check_result();
}
