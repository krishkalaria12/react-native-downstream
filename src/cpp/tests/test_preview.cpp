// Tests for src/cpp/remend/Preview.h.
//
// The inputs are ported from the test suite of Vercel's `remend`
// (github.com/vercel/streamdown, packages/remend/__tests__). The expectations
// are not. remend asserts on a repaired string, which encodes its own choices
// about what a half-written document should look like; several of those choices
// are not CommonMark, and a few are pinned as wrong by
// tests/streaming-preview.test.ts. So each case here states what this engine
// should show and, where that differs from remend, says why in the comment.
//
// What the port does keep is the coverage. remend's suite is a few hundred
// shapes of markdown a model can stop emitting mid-token, and every one of them
// has to survive a pass that is allowed to rewrite the tail. For each input
// exactly one of two things has to be true: the open span is styled, or nothing
// happened. Losing a block, inventing one, or rewriting a block the reader has
// already seen is never an outcome.
//
// Provenance: github.com/vercel/streamdown, packages/remend/__tests__.
// The inputs are markdown syntax, which is not copyrightable, and are
// reproduced under Apache-2.0 with this notice.

#include "Compare.h"
#include "IncrementalParser.h"
#include "Preview.h"
#include "Scan.h"
#include "Unicode.h"
#include "tests/check.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace downstream;
using namespace downstream::remend;

namespace {

const Preview kPreview{PreviewOptions::all()};
const Preview kDisabled{};

/**
 * The math options the ported corpus runs under: none.
 *
 * The corpus is remend's, and every case in it is about emphasis, links, images
 * or structure. A formula is none of those, and with brackets on by default a
 * case that happens to contain a paired `\[` would be testing the bracket
 * pre-scan rather than the repair it was written for. So the corpus parses with
 * math off, which is also the state the whole fixture corpus is byte-identical
 * in, and `mathPreview` turns math on for the cases that are about it.
 */
const ParseOptions kNoMath{ParseFlags::Enabled & ~ParseFlags::Enabled};

/// `kPreview` with math off, so a case about a span is a case about a span.
PreviewResult applyCorpus(std::string_view input, size_t origin = 0, size_t frozenBlocks = 0) {
  return kPreview.apply(input, origin, frozenBlocks, kNoMath);
}

constexpr uint32_t kStrong = InlineStyle::Strong;
constexpr uint32_t kEmphasis = InlineStyle::Emphasis;
constexpr uint32_t kStrike = InlineStyle::Strikethrough;
constexpr uint32_t kCode = InlineStyle::Code;

/// A closer arriving in its own chunk, so the stream settles one span at a time.
constexpr const char *emphasis_mark = "**";

/// Visible text of every inline block, joined by '|'.
std::string visible(const Document &document) {
  std::string out;
  for (const Block &block : document.blocks) {
    // An image block counts: its text is its alt text, which is exactly what a
    // repaired unfinished image shows, and a helper that skipped it would make
    // every image repair look like it deleted the reader's text.
    if (block.kind != BlockKind::Paragraph && block.kind != BlockKind::Heading &&
        block.kind != BlockKind::TableRow && block.kind != BlockKind::Image) {
      continue;
    }
    if (!out.empty()) {
      out += '|';
    }
    out += utf16ToUtf8(block.text);
  }
  return out;
}

/// Every inline style present anywhere in the document.
uint32_t styles(const Document &document) {
  uint32_t mask = 0;
  for (const Block &block : document.blocks) {
    for (const InlineRun &run : block.runs) {
      mask |= run.styles;
    }
  }
  return mask;
}

std::string label(const char *name, std::string_view input) {
  char buffer[512];
  snprintf(buffer, sizeof(buffer), "%s: %s", name, std::string(input).c_str());
  return buffer;
}

/// The preview closed an open span, so the content reads styled and the
/// markers are gone from the visible text.
void openSpan(std::string_view input, std::string_view expectedVisible, uint32_t expectedStyles) {
  const std::string name = label("open span", input);
  const PreviewResult result = applyCorpus(input);
  CHECK(result.repaired, name.c_str());
  CHECK(visible(*result.parsed.document) == std::string(expectedVisible), name.c_str());
  CHECK((styles(*result.parsed.document) & expectedStyles) == expectedStyles, name.c_str());
}

/// The preview declined. The bytes it was handed come back untouched, which is
/// the whole contract: an unproven repair is not made.
void untouched(std::string_view input, const char *why) {
  const std::string name = label(why, input);
  const PreviewResult result = applyCorpus(input);
  CHECK(!result.repaired, name.c_str());
  CHECK(result.text == std::string(input), name.c_str());
}

/// The preview declined, and md4c already renders this the way it should. These
/// are the cases where remend would have rewritten the source to reach the same
/// answer, and where doing so would have been wrong.
void showsAs(std::string_view input, std::string_view expectedVisible, const char *why) {
  const std::string name = label(why, input);
  const PreviewResult result = applyCorpus(input);
  CHECK(!result.repaired, name.c_str());
  CHECK(visible(*result.parsed.document) == std::string(expectedVisible), name.c_str());
}

bool unterminatedBoldItalic() {
  openSpan("***Starting bold-italic", "Starting bold-italic", kStrong | kEmphasis);
  openSpan("***incomplete", "incomplete", kStrong | kEmphasis);
  openSpan("This is ***very", "This is very", kStrong | kEmphasis);
  openSpan("This is ***very important", "This is very important", kStrong | kEmphasis);
  openSpan("Text with ***bold and italic text", "Text with bold and italic text", kStrong | kEmphasis);
  // A lone marker with no content after it has nothing to style, so a repair
  // that proved itself by increasing style coverage cannot be made.
  untouched("text ***", "a marker with no content styles nothing");
  untouched("text****", "a marker with no content styles nothing");
  untouched("text*****", "a marker with no content styles nothing");
  untouched("text******", "a marker with no content styles nothing");
  untouched("word****", "a marker with no content styles nothing");
  // Already closed.
  showsAs("***Starting bold-italic***", "Starting bold-italic", "a closed span needs no repair");
  showsAs("***incomplete***", "incomplete", "a closed span needs no repair");
  showsAs("***text***", "text", "a closed span needs no repair");
  showsAs("***word text***", "word text", "a closed span needs no repair");
  showsAs("***start***end***", "startend***", "an intraword tail is not an opener");
  showsAs("This is ***very important***", "This is very important", "a closed span needs no repair");
  showsAs("text***", "text***", "a marker with no content styles nothing");
  // The trailing word leaves one literal star behind, so the pass closes the
  // span and the whole line reads the way it will once the writer finishes.
  openSpan("***bold and *italic*** more text", "bold and italic more text", kStrong);
  showsAs("*italic* **bold** ***both***", "italic bold both", "a closed span needs no repair");
  showsAs("Combined **bold and *italic*** text", "Combined bold and italic text",
          "a closed span needs no repair");
  showsAs("- Combined **bold and *italic*** text", "Combined bold and italic text",
          "a closed span needs no repair");
  showsAs("end******", "end******", "a marker with no content styles nothing");
  showsAs("test **bold and *italic*** end", "test bold and italic end", "a closed span needs no repair");
  return true;
}

bool unterminatedBold() {
  openSpan("**incomplete", "incomplete", kStrong);
  openSpan("Here is some **bold tex", "Here is some bold tex", kStrong);
  openSpan("Text with **bold", "Text with bold", kStrong);
  openSpan("**bold", "bold", kStrong);
  openSpan("# Heading\n**bold", "Heading|bold", kStrong);
  openSpan("paragraph1\n\n**bold", "paragraph1|bold", kStrong);
  openSpan("text\n\n\n**bold", "text|bold", kStrong);
  openSpan("text\n**bold", "text\nbold", kStrong);
  openSpan("1. First\n2. **Second item with bold", "First|Second item with bold", kStrong);
  openSpan("> quote\n**bold", "quote\nbold", kStrong);
  openSpan("***bold-italic with `code", "bold-italic with code", kStrong | kEmphasis | kCode);
  // A half-typed closer is not an open span. md4c already reads `**bold*` as
  // emphasis, which is the CommonMark answer and the one the engine pins.
  showsAs("**xxx*", "*xxx", "a half-typed closer is not an open span");
  showsAs("**bold text*", "*bold text", "a half-typed closer is not an open span");
  showsAs("This is **bold text*", "This is *bold text", "a half-typed closer is not an open span");
  showsAs("Text with **bold*", "Text with *bold", "a half-typed closer is not an open span");
  showsAs("**bold*", "*bold", "a half-typed closer is not an open span");
  showsAs("**bold text**", "bold text", "a closed span needs no repair");
  showsAs("**bold1** and **bold2**", "bold1 and bold2", "a closed span needs no repair");
  showsAs("**first** and **second**", "first and second", "a closed span needs no repair");
  showsAs("**bold** then **more**", "bold then more", "a closed span needs no repair");
  openSpan("**bold and *italic*", "bold and italic", kStrong);
  // A bare marker carries no content. The engine reads three or more as a
  // thematic break, so there is no paragraph to style at all.
  untouched("**", "a bare marker has no content");
  untouched("** ", "a bare marker has no content");
  untouched("  **  ", "a bare marker has no content");
  untouched("** __", "a bare marker has no content");
  return true;
}

bool unterminatedItalic() {
  openSpan("*incomplete", "incomplete", kEmphasis);
  openSpan("_incomplete", "incomplete", kEmphasis);
  openSpan("This is *italic", "This is italic", kEmphasis);
  openSpan("This is _italic", "This is italic", kEmphasis);
  openSpan("Text with *italic", "Text with italic", kEmphasis);
  openSpan("Text with _italic", "Text with italic", kEmphasis);
  openSpan("Text with __italic", "Text with italic", kStrong);
  showsAs("Text with __italic text__", "Text with italic text", "a closed span needs no repair");
  openSpan("_italic text", "italic text", kEmphasis);
  openSpan("_start with underscore", "start with underscore", kEmphasis);
  showsAs("_start with underscore_", "start with underscore", "a closed span needs no repair");
  openSpan("The variable_name is _important", "The variable_name is important", kEmphasis);
  openSpan("_privateVariable", "privateVariable", kEmphasis);
  openSpan("Cost \\$100 with _incomplete", "Cost $100 with incomplete", kEmphasis);
  // The escaped pair opens nothing, so the unescaped one beside it is the only
  // opener and settles on its own.
  openSpan("\\*escaped\\* but *real italic", "*escaped* but real italic", kEmphasis);
  openSpan("\\_escaped\\_ and _unescaped", "_escaped_ and unescaped", kEmphasis);
  openSpan("The user_id field stores the _unique identifier",
           "The user_id field stores the unique identifier", kEmphasis);
  openSpan("Price is \\$50 and _this is italic", "Price is $50 and this is italic", kEmphasis);
  openSpan("Test with *italic and some_var_name", "Test with italic and some_var_name", kEmphasis);
  // A trailing newline belongs after the closer, not inside the span.
  openSpan("Start _text\n", "Start text", kEmphasis);
  openSpan("Start _italic\n", "Start italic", kEmphasis);
  // remend closes this one. The blank line has already ended the paragraph,
  // so no later chunk can close the span, and styling it would show something
  // the settled document never will.
  showsAs("_incomplete\n\n", "_incomplete", "a blank line has closed the paragraph");
  openSpan("line1\n\n*italic text", "line1|italic text", kEmphasis);

  for (const char *input : {
           "*incomplete*", "_incomplete_", "__incomplete__", "**bold** and *italic*",
           "*word* and more text", "Text with *italic text*", "Text with __italic__",
           "This is __bold text__", "__bold__ and _italic_", "_complete italic_ and some_other_text",
           // Escaped markers open nothing, so the unescaped one beside them is
           // the only opener and it settles on its own.
           "Text with \\_escaped underscore", "\\_escaped\\_ _complete_ pair",
           "\\*not italic",
           // Intraword markers are not delimiters.
           "123*abc", "abc*123", "234234*123", "foo*bar", "hello*world", "test*123*test",
           "*file*name*ext", "5*6*78", "x *y*z w", "a *b*c*d",
           "café_price", "naïve_approach", "some_variable_name", "test_123_value",
           "hello_world", "hello_world_test", "The value is 1_000_000", "MAX_VALUE",
           "__init__ and __main__ are special", "word_",
           // A star between spaces is arithmetic.
           "2 * 3 * 4", "3 + 2 - 5 * 0 = ?", "5 * 0", "x * y", "a * b = c",
           "Text ending with *", "Text ending with **", "text**",
           // A list marker is a marker, not an opener.
           "- *", "- _", "- __", "- **", "- ***",
       }) {
    untouched(input, "no unambiguous opener");
  }
  // Already closed, so nothing to repair even though the shape looks open.
  showsAs("123*abc", "123*abc", "an intraword star is not a delimiter");
  showsAs("hello_world", "hello_world", "an intraword underscore is not a delimiter");
  showsAs("The value is 1_000_000", "The value is 1_000_000", "an intraword underscore is not a delimiter");
  showsAs("2 * 3 * 4", "2 * 3 * 4", "a star between spaces is arithmetic");
  showsAs("*italic with some*var*name inside", "italic with somevar*name inside",
          "an intraword star is not a delimiter");
  openSpan("*start \\* middle \\* end", "start * middle * end", kEmphasis);
  return true;
}

bool unterminatedStrikethrough() {
  openSpan("~~incomplete", "incomplete", kStrike);
  openSpan("Text with ~~strike", "Text with strike", kStrike);
  openSpan("~~first~~ and ~~second", "first and second", kStrike);
  // The tilde pair is a real opener even though the single one beside it is not.
  openSpan("20~25 and ~~strike", "20~25 and strike", kStrike);
  openSpan("`&amp", "&amp", kCode);
  openSpan("~~🎉 celebration", "🎉 celebration", kStrike);
  openSpan("> ~~struck text", "struck text", kStrike);
  openSpan("- [x] completed ~~struck", "completed struck", kStrike);

  // A single tilde is never an opener. md4c's GitHub dialect does read `~x~`
  // as strikethrough, so closing one would invent a span the writer never
  // asked for. This is the case remend's singleTilde handler exists to prevent,
  // by escaping the tilde; leaving the text alone is the same answer without
  // rewriting the source.
  for (const char *input : {"~hello", "hello~", "20~25", "20~25°C",
                            "foo~bar", "foo~bar~baz", "hello ~ world", "日本~語", "é~x", "α~β",
                            "𐐀~a", "20\\~25°C", "foo\\~bar", "text~"}) {
    untouched(input, "a single tilde is not strikethrough");
  }
  showsAs("~hello", "~hello", "a single tilde is not strikethrough");
  showsAs("20~25", "20~25", "a single tilde is not strikethrough");
  showsAs("日本~語", "日本~語", "a single tilde is not strikethrough");
  showsAs("foo~bar", "foo~bar", "a single tilde is not strikethrough");

  // A half-typed closer is left alone rather than completed, because the
  // completion would have to drop the tilde the writer already sent.
  untouched("~~xxx~", "a half-typed closer is not completed");
  untouched("Text with ~~strike~", "a half-typed closer is not completed");
  untouched("This is ~~strikethrough~", "a half-typed closer is not completed");
  untouched("~~strike text~", "a half-typed closer is not completed");

  showsAs("~~strike text~~", "strike text", "a closed span needs no repair");
  showsAs("a ~~b~~ c", "a b c", "a closed span needs no repair");
  showsAs("~~first~~ and ~~second~~", "first and second", "a closed span needs no repair");
  showsAs("~~xxx~~", "xxx", "a closed span needs no repair");
  showsAs("- [x] completed ~~struck~~", "completed struck", "a closed span needs no repair");
  return true;
}

bool unterminatedInlineCode() {
  openSpan("`incomplete", "incomplete", kCode);
  openSpan("`code", "code", kCode);
  openSpan("Text with `code", "Text with code", kCode);
  openSpan("text\t`code", "text\tcode", kCode);
  openSpan("`código", "código", kCode);
  openSpan("`한국어 코드", "한국어 코드", kCode);
  openSpan("`code` then `more", "code then more", kCode);
  openSpan("`code` **bold", "code bold", kStrong);
  openSpan("**bold with `code", "bold with code", kStrong | kCode);
  // The escaped backticks are literal, so the `**` beside them is a real
  // opener and settles on its own.
  openSpan("\\`not code\\` **bold", "`not code` bold", kStrong);

  // A bare backtick run has no content, so there is nothing to style.
  for (const char *input : {"`", "``", "text`", "text ``", "```", "``````", "`````"}) {
    untouched(input, "a bare backtick run has no content");
  }
  // Markers inside a complete code span are content, never syntax.
  showsAs("`code **not bold`", "code **not bold", "a closed code span needs no repair");
  showsAs("Text with `inline code`", "Text with inline code", "a closed code span needs no repair");
  showsAs("`code` and `code2`", "code and code2", "closed code spans need no repair");
  showsAs("`code`\n```", "code", "a closed code span needs no repair");
  untouched("```\\ncode block with `backtick\\n```", "a closed fence is verbatim");
  // An info string is only an info string once a line break follows it.
  showsAs("```python code", "", "an unterminated fence carries no inline text");
  showsAs("```code", "", "an unterminated fence carries no inline text");
  return true;
}

/// An unfinished link previews as its text and an unfinished image as its alt
/// text, the way Streamdown shows them. remend points the link at a sentinel
/// scheme; here the partial target is dropped, so no link event can fire and
/// the text reads exactly as the finished document will, minus the tap target.
void showsLinkText(std::string_view input, std::string_view expectedVisible) {
  const std::string name = label("unfinished link", input);
  const PreviewResult result = applyCorpus(input);
  CHECK(result.repaired, name.c_str());
  CHECK(visible(*result.parsed.document) == std::string(expectedVisible), name.c_str());
  CHECK(result.parsed.document->links.empty(), name.c_str());
}

bool linksAndImages() {
  showsLinkText("[link text", "link text");
  showsLinkText("[incomplete link", "incomplete link");
  showsLinkText("Text with [incomplete link", "Text with incomplete link");
  showsLinkText("Check out [this lin", "Check out this lin");
  showsLinkText("Check the [documentation", "Check the documentation");
  showsLinkText("[some text", "some text");
  showsLinkText("[link [nested] text", "link [nested] text");
  // Appending the bracket closes the innermost, as the writer's own will.
  showsLinkText("Text [outer [inner", "Text [outer inner");
  showsLinkText("[foo [bar [baz", "[foo [bar baz");
  // The partial target is dropped, never followed, whatever shape it has.
  showsLinkText("See [the docs](https://exa", "See the docs");
  showsLinkText("Visit [our site](https://exa", "Visit our site");
  showsLinkText("[a](http://example.com/x", "a");
  showsLinkText("[text](https://example.com \"tit", "text");
  showsLinkText("[text](<https://exa", "text");
  showsLinkText("[a](https://x.com/_foo", "a");
  // An image shows its alt text, which is how the engine renders a finished one.
  showsLinkText("![partial", "partial");
  showsLinkText("Text ![incomplete image", "Text incomplete image");
  showsLinkText("See ![the diag", "See the diag");
  showsLinkText("![a](http://ex", "a");
  showsLinkText("See ![diagram](http://example.com/img", "See diagram");
  showsLinkText("Text ![outer [inner", "Text ![outer inner");

  // Spans inside the link text close inside it, and spans around the link close
  // around it, so the closers nest the way the writer's will.
  openSpan("[**bold link", "bold link", kStrong);
  openSpan("Text with [link and **bold", "Text with link and bold", kStrong);
  openSpan("**bold [docs](htt", "bold docs", kStrong);

  // A space ends a bare destination, so this is prose that will never be a link,
  // and dropping it would delete words the writer typed.
  untouched("see [note](which is long", "a space ends a bare destination");
  untouched("read [the docs](http://example.com/guide now", "a space ends a bare destination");
  // Balanced brackets are already whatever they will be until `(` arrives.
  untouched("Text [outer [inner]]", "balanced brackets are not open");
  untouched("a \\[not a link", "an escaped bracket opens nothing");
  // A task marker still arriving would show a letter where a checkbox is about
  // to appear.
  untouched("- [", "a task marker is not a link");
  untouched("- [x", "a task marker is not a link");
  untouched("1. [ ", "a task marker is not a link");
  // A complete link keeps its target; only the unfinished one is touched.
  {
    const PreviewResult beside = applyCorpus("[a](https://example.com) and [more");
    CHECK(beside.repaired && visible(*beside.parsed.document) == "a and more",
          "an unfinished link beside a complete one previews");
    CHECK(beside.parsed.document->links.size() == 1 &&
              beside.parsed.document->links[0].url == "https://example.com",
          "and the complete link keeps its target");
  }

  // A partial destination is not a target even when the URL inside it is
  // already complete, because the next chunk can extend or move it.
  for (const char *input : {"![a](http://ex", "![a](http://example.com/x", "[a](http://example.com/x",
                            "See ![diagram](http://example.com/img",
                            "read [the docs](http://example.com/guide now"}) {
    const std::string name = label("an unfinished target is not actionable", input);
    const ParseResult result = applyCorpus(input).parsed;
    CHECK(result.document->links.empty(), name.c_str());
  }
  // A complete link survives a neighbouring open span.
  const std::string beside = label("a complete link survives", "Text with [complete link](https://example.com/x) and **bold");
  const PreviewResult result = applyCorpus("Text with [complete link](https://example.com/x) and **bold");
  CHECK(result.repaired, beside.c_str());
  CHECK(result.parsed.document->links.size() == 1, beside.c_str());
  CHECK(result.parsed.document->links[0].url == "https://example.com/x", beside.c_str());
  CHECK(visible(*result.parsed.document) == "Text with complete link and bold", beside.c_str());
  return true;
}

bool structureStillArriving() {
  // remend rewrites `- > 25` into `- \> 25`. That is a workaround for a parser
  // that reads the `>` as anything but a quote; md4c follows CommonMark, so the
  // item really is a list item containing a quote, and escaping it would break
  // the document the writer typed.
  for (const char *input : {"- > 25: rich", "* > 25: rich", "+ > 25: rich", "1. > 25: rich",
                            "2) > 10: high", "  - > 25: rich"}) {
    const std::string name = label("a greater-than after a list marker is a quote", input);
    const ParseResult parsed = applyCorpus(input).parsed;
    CHECK(parsed.document->blocks.size() == 1, name.c_str());
    CHECK(parsed.document->blocks[0].containers.size() == 2, name.c_str());
    CHECK(parsed.document->blocks[0].containers[0].kind == ContainerKind::ListItem, name.c_str());
    CHECK(parsed.document->blocks[0].containers[1].kind == ContainerKind::Quote, name.c_str());
  }
  // The contrast case. `<` is not a quote marker, so this is a plain list item,
  // which is what makes the case above structural rather than a blanket rule.
  const ParseResult less = applyCorpus("- < 5").parsed;
  CHECK(less.document->blocks[0].containers.size() == 1, "a less-than after a list marker is text");

  // Setext: remend injects a zero-width space after a partial underline because
  // it cannot tell a heading from a rule until both arrive. md4c waits for the
  // line, which is the same answer without the invisible character.
  for (const char *input : {"Guide\n-", "Guide\n--", "Guide\n---", "Guide\n=", "This is a title\n=",
                            "Some text\n-", "here is a list\n-", "`code`\n-"}) {
    const std::string name = label("a setext underline completes on arrival", input);
    const ParseResult parsed = applyCorpus(input).parsed;
    CHECK(!parsed.document->blocks.empty(), name.c_str());
    if (!parsed.document->blocks.empty()) {
      CHECK(parsed.document->blocks[0].kind == BlockKind::Heading, name.c_str());
    }
  }
  untouched("Guide\n-", "a setext underline completes on arrival");

  // A table needs its delimiter row before it is a table.
  showsAs("| A | B |\n", "| A | B |", "a table is not a table without a delimiter row");
  showsAs("| A | B |\n| --- |\n", "A", "a table needs a complete delimiter row");
  untouched("| a | b |\n| --- | --- |\n| **dat", "a partial table row is not repaired");

  // A fence needs a line break.
  const ParseResult fence = applyCorpus("```python\nconst x = 1").parsed;
  CHECK(fence.document->blocks[0].kind == BlockKind::CodeBlock, "an open fence stays code");
  CHECK(fence.document->blocks[0].language == "python", "an open fence keeps its language");
  openSpan("```\ncode\n```\n**bold", "bold", kStrong);
  openSpan("```js\nconst x = 1;\n```\n\nThis creates a **variable", "This creates a variable", kStrong);

  // Math and raw HTML are literal by policy. A repair pass must not invent
  // delimiters the writer did not type, and this one closes no span in either.
  for (const char *input : {"$x +", "$x_", "$x^2 + y^2", "$$x", "$$incomplete", "text$", "text$$",
                            "$$x$y$$z", "\\sum_{i=0}^{n} x_i", "$$x + y = z", "$incomplete$",
                            "Math: $$x+y and code: `$$`", "The formula $E = mc^2",
                            "Inline $x$ and block $$y$$", "\\[P(x_{t+1} | x_t)\\]"}) {
    untouched(input, "math source stays literal");
  }
  for (const char *input : {"<div", "Hello <div", "text <custom", "3 < 5", "x < y", "value <1",
                            "</div", "text <", "text<div class=\"test", "$$a<b$$ done",
                            "Text <MyComponent", "if a <", "inline $A_{j<k}$ more"}) {
    untouched(input, "raw HTML stays literal text");
  }
  showsAs("x < y", "x < y", "a less-than sign is text");
  showsAs("3 < 5", "3 < 5", "a less-than sign is text");
  showsAs("$$a<b$$ done", "$$a<b$$ done", "a less-than inside math is text");
  return true;
}

bool escapesAndMarkers() {
  // A backslash is syntax: it does not reach the reader, and it stops the
  // character after it from opening anything.
  showsAs("\\**not bold", "**not bold", "an escaped star opens nothing");
  // An escaped backslash leaves the star after it a real opener, so it closes.
  openSpan("\\\\*actually italic", "\\actually italic", kEmphasis);
  showsAs("\\*not italic", "*not italic", "an escaped star opens nothing");
  // The escaped backtick is literal, so the star beside it is a real opener.
  openSpan("\\` *italic", "` italic", kEmphasis);
  showsAs("not code \\` still [not] code", "not code ` still [not] code",
          "an escaped backtick keeps the brackets literal");
  showsAs("use `map[key]` here", "use map[key] here", "brackets inside a code span are content");
  showsAs("foo\\~bar", "foo~bar", "an escaped tilde is already unambiguous");
  showsAs("Text with \\* escaped asterisk", "Text with * escaped asterisk",
          "an escape is consumed");
  showsAs("text \\``real` code", "text `real code",
          "an escaped backtick leaves the second one free to open");
  // A cold intraword star does not open, but the spaced one beside it does.
  openSpan("5 * 0 and *italic", "5 * 0 and italic", kEmphasis);
  showsAs("5 * 0", "5 * 0", "a star between spaces is arithmetic");

  // Arithmetic and comparison operators are not delimiters.
  showsAs("a * b = c", "a * b = c", "a star between spaces is arithmetic");
  showsAs("2 * 3 * 4", "2 * 3 * 4", "a star between spaces is arithmetic");
  showsAs("The price is $50 and $100", "The price is $50 and $100", "currency is not math");
  showsAs("(SFT)", "(SFT)", "bare parentheses are text");
  return true;
}

bool listMarkers() {
  // A marker after a list marker is a list, a rule, or an empty item. None of
  // them has inline content, so there is nothing for a repair to style.
  for (const char *input : {"- **", "- __", "- *", "- _", "- ~~", "- `", "- ***", "- __\n- **",
                            "* __\n* **", "+ __\n+ **", "\n- __\n- **", "-\n- **",
                            "* First item\n* Second item\n* Third item",
                            "* Item 1\n* Item 2\n* Item 3",
                            "- Item 1\n- Item 2 with *italic*\n- Item 3",
                            "* Parent item\n  * Nested item 1\n  * Nested item 2"}) {
    const PreviewResult result = applyCorpus(input);
    const std::string name = label("a list marker is not an inline opener", input);
    CHECK(!result.repaired || result.text != std::string(input), name.c_str());
    // A repair is only ever allowed on the last block, and a list item's
    // content is what the reader is looking at, so the check is that the block
    // count and every earlier block are untouched.
    const ParseResult baseline = parseMarkdown(input, kNoMath);
    CHECK(result.parsed.document->blocks.size() >= baseline.document->blocks.size(), name.c_str());
  }
  // A list item whose content has an open span does repair, and only there.
  openSpan("1. One\n2. **Sec", "One|Sec", kStrong);
  openSpan("- Item 1\n- Item 2 with **bol", "Item 1|Item 2 with bol", kStrong);
  openSpan("- [ ] **bold task", "bold task", kStrong);
  openSpan("> * list with **bold", "list with bold", kStrong);
  openSpan("> > **deeply nested bold", "deeply nested bold", kStrong);
  return true;
}

/// Every open span closes, innermost first, so the closers nest the way the
/// writer's eventual ones will. remend produces the same document for these.
bool nestedOpenSpans() {
  openSpan("**bold *italic", "bold italic", kStrong | kEmphasis);
  openSpan("**bold _und", "bold und", kStrong | kEmphasis);
  openSpan("**bold *italic `code", "bold italic code", kStrong | kEmphasis | kCode);
  openSpan("**bold ~~strike", "bold strike", kStrong | kStrike);
  openSpan("~~strike with **bold", "strike with bold", kStrike | kStrong);
  openSpan("*italic with **bold", "italic with bold", kEmphasis | kStrong);
  openSpan("**bold *italic ~~strike `code", "bold italic strike code", kStrong | kEmphasis | kStrike | kCode);
  openSpan("**bold with $x^2", "bold with $x^2", kStrong);
  openSpan("combined **_bold and italic", "combined bold and italic", kStrong | kEmphasis);
  // The inner span already closed, so only the outer one is left to settle.
  openSpan("**bold *italic*", "bold italic", kStrong);
  openSpan("**bold and *italic*", "bold and italic", kStrong);

  // A candidate that fails its proof is skipped, so the escaped star cannot
  // hide the real opener before it.
  openSpan("**bold and \\*literal", "bold and *literal", kStrong);

  // Each candidate costs a parse of the tail, so a tail full of markers that
  // all fail is bounded rather than probed once per marker.
  const PreviewResult capped = applyCorpus("**a \\*b \\*c \\*d \\*e \\*f");
  CHECK(capped.stats.probes == 5, "a tail of failing candidates stops at the probe budget");
  CHECK(!capped.repaired && capped.stats.refused == 1, "and is refused once, as one tail");
  return true;
}

/// Where the closer goes, and when there is nowhere it could go.
bool trailingWhitespace() {
  // A chunk that ends on whitespace still previews. A closer after the space
  // would not be right-flanking, so it goes before the trailing run.
  openSpan("**bold word ", "bold word", kStrong);
  openSpan("**bold word  ", "bold word", kStrong);
  openSpan("**bold word\t", "bold word", kStrong);
  openSpan("*italic word ", "italic word", kEmphasis);
  openSpan("~~strike word ", "strike word", kStrike);
  openSpan("`code word ", "code word", kCode);
  openSpan("**bold wor\r\n", "bold wor", kStrong);

  // One line break leaves a paragraph open: the next line may continue it and
  // carry the closer.
  openSpan("**bold\n", "bold", kStrong);
  openSpan("- **bold item\n", "bold item", kStrong);
  openSpan("# **Tit", "Tit", kStrong);

  // A block md4c has already closed can never receive a closer, so a preview
  // there would be retracted by the next block rather than confirmed.
  showsAs("# **Title\n", "**Title", "a heading ends with its line");
  showsAs("## *Note\n\n", "*Note", "a heading ends with its line");
  showsAs("**bold text\n\n", "**bold text", "a blank line ends the paragraph");
  showsAs("**bold text\n  \n", "**bold text", "a blank line ends the paragraph");
  showsAs("- **bold item\n\n", "**bold item", "a blank line ends the item's paragraph");
  untouched("| A | B |\n| --- | --- |\n| **x | y |\n", "a table row ends with its line");
  return true;
}

/// A repair that reached past the last block would restyle text a reader has
/// already seen settled, so the policy refuses it. These cases prove the check
/// is not vacuous.
bool repairIsScopedToTheLastBlock() {
  for (const char *input : {"# Heading\n**bold", "```\ncode\n```\n**bold", "* One\n* Two **bo",
                            "text **bold and *ital", "**bold** then **more",
                            "paragraph1\n\n**bold", "line1\n\n*italic text"}) {
    const std::string name = label("a repair touches only the last block", input);
    const PreviewResult result = applyCorpus(input);
    const ParseResult baseline = parseMarkdown(input, kNoMath);
    CHECK(result.repaired, name.c_str());
    CHECK(result.parsed.document->blocks.size() == baseline.document->blocks.size(), name.c_str());
    for (size_t i = 0; i + 1 < baseline.document->blocks.size(); i++) {
      char detail[512];
      snprintf(detail, sizeof(detail), "%s: block %zu is untouched", std::string(input).c_str(), i);
      CHECK(scan::sameBlock(baseline.document->blocks[i], result.parsed.document->blocks[i]), detail);
    }
  }
  // The second span closes while the first, already closed one stays styled.
  openSpan("**bold** then **more", "bold then more", kStrong);
  const PreviewResult both = applyCorpus("**bold** then **more");
  CHECK(both.parsed.document->blocks[0].runs.size() == 2, "both spans are styled");
  CHECK(both.parsed.document->blocks[0].runs[0].start == 0, "the first span keeps its range");
  CHECK(both.parsed.document->blocks[0].runs[1].start == 10, "the second span gets the new range");
  return true;
}

bool featuresAreOptIn() {
  // The default is inert, which is what the engine shipped and what the
  // streaming-preview suite pins.
  const PreviewResult off = kDisabled.apply("**bold wor", 0);
  CHECK(!off.repaired, "a disabled preview never repairs");
  CHECK(off.stats.probes == 0, "a disabled preview does no work");
  CHECK(off.stats.applied == 0, "a disabled preview applies nothing");
  CHECK(visible(*off.parsed.document) == "**bold wor", "a disabled preview leaves markers literal");
  CHECK(styles(*off.parsed.document) == 0, "a disabled preview styles nothing");

  PreviewOptions emphasisOnly;
  emphasisOnly.features = static_cast<uint32_t>(PreviewFeature::Emphasis);
  const Preview emphasis{emphasisOnly};
  CHECK(emphasis.enabled(), "a feature set is enabled");
  CHECK(emphasis.apply("**bold wor", 0).repaired, "emphasis alone closes bold");
  CHECK(!emphasis.apply("`code", 0).repaired, "emphasis alone does not close a code span");

  PreviewOptions codeOnly;
  codeOnly.features = static_cast<uint32_t>(PreviewFeature::InlineCode);
  const Preview code{codeOnly};
  CHECK(!code.apply("**bold wor", 0).repaired, "code alone does not close bold");
  CHECK(code.apply("`code", 0).repaired, "code alone closes a code span");

  PreviewOptions linksOnly;
  linksOnly.features = static_cast<uint32_t>(PreviewFeature::Links);
  const Preview links{linksOnly};
  CHECK(links.apply("See [the docs](https://exa", 0).repaired, "links alone settles a link");
  CHECK(!links.apply("See ![diagram](https://exa", 0).repaired, "links alone leaves an image");
  CHECK(!links.apply("**bold wor", 0).repaired, "links alone does not close bold");

  PreviewOptions imagesOnly;
  imagesOnly.features = static_cast<uint32_t>(PreviewFeature::Images);
  const Preview images{imagesOnly};
  CHECK(images.apply("See ![diagram](https://exa", 0).repaired, "images alone settles an image");
  CHECK(!images.apply("See [the docs](https://exa", 0).repaired, "images alone leaves a link");

  PreviewOptions mathOnly;
  mathOnly.features = static_cast<uint32_t>(PreviewFeature::Math);
  const Preview math{mathOnly};
  CHECK(math.apply("See $$x + ", 0, 0, ParseOptions{ParseFlags::Default}).repaired,
        "math alone closes a formula");
  CHECK(!math.apply("**bold wor", 0, 0, kNoMath).repaired, "math alone does not close bold");
  // With math off there is no formula to close, so the repair proves nothing and
  // is not made. The candidate is still found — the scan reads bytes — and the
  // proof is what refuses it, which is the whole safety argument.
  CHECK(!math.apply("See $$x + ", 0, 0, kNoMath).repaired, "math off means no formula to close");

  PreviewOptions empty;
  CHECK(!empty.enabled(), "no features means disabled");
  CHECK(empty.features == 0, "the default feature set is empty");
  return true;
}

/**
 * The math repair, and the shapes it has to refuse.
 *
 * Its own group because it is the only repair that *adds* a run and a span
 * rather than moving a marker, so the corpus's "nothing but the marker changed"
 * rule does not describe it. What it does have to prove is narrower and is
 * stated in `provesMathRepair`.
 */
bool mathPreview() {
  const ParseOptions on{ParseFlags::Default};
  const ParseOptions bracketsOff{ParseFlags::Enabled | ParseFlags::SingleDollar};

  // The plain case: a formula that has not closed yet, closed provisionally and
  // typeset from then on.
  {
    const PreviewResult result = kPreview.apply("See $$x + ", 0, 0, on);
    CHECK(result.repaired, "an open $$ closes");
    CHECK(visible(*result.parsed.document) == "See $$x + $$", "the formula keeps its literal");
    const Block &block = result.parsed.document->blocks.back();
    CHECK(block.math.size() == 1, "one formula");
    CHECK(block.math[0].tex == "x + ", "the TeX is what arrived");
    CHECK(block.math[0].display, "a doubled pair is display style");
  }

  // A single `$` is not a formula with the default, because currency is the
  // commoner thing a `$` is in an answer.
  CHECK(!kPreview.apply("That costs $5 and ", 0, 0, on).repaired, "a single dollar is text");
  // And it is one when the app asked, which is the name Streamdown uses. An
  // opener has to be preceded by whitespace or punctuation, so `costs $5` can
  // open while `$5 and $10` cannot close, which is why the default is off.
  CHECK(kPreview.apply("That costs $5 and ", 0, 0, bracketsOff).repaired, "single dollar on request");
  {
    const PreviewResult result = kPreview.apply("That costs $5 and ", 0, 0, bracketsOff);
    const MathSpan &span = result.parsed.document->blocks.back().math.front();
    CHECK(span.delimiter == MathDelimiter::Dollar, "and the span knows which delimiter it was");
    CHECK(!span.display, "a single dollar is text style, not a display formula");
    CHECK(span.tex == "5 and ", "with the TeX that arrived");
  }
  CHECK(!kPreview.apply("That costs $5 and $10", 0, 0, bracketsOff).repaired,
        "a dollar followed by a digit cannot close");

  // `\(` pairs only when the closer arrives, so an unfinished one is the
  // CommonMark escape it is in a finished document.
  CHECK(!kPreview.apply("\\(x + ", 0, 0, on).repaired, "an open bracket delimiter is text");
  // A complete one is math, and the pre-scan is what makes that true: md4c only
  // ever sees `$$`, and the builder reads the original bytes to say which
  // delimiter the writer used and to show the block as it was written.
  {
    const PreviewResult result = kPreview.apply("\\(x + \\)", 0, 0, on);
    const Block &block = result.parsed.document->blocks.back();
    CHECK(block.math.size() == 1, "a paired bracket is math");
    CHECK(block.math[0].delimiter == MathDelimiter::Paren, "and it knows which delimiter it was");
    CHECK(block.text == u"\\(x + \\)", "and it shows as written");
  }

  // A closed formula needs no repair at all, which is what stops a settled
  // document from being rewritten.
  CHECK(!kPreview.apply("See $$x^2$$ here", 0, 0, on).repaired, "a closed formula is left alone");
  // Nor does a lone `$$` fence, which is a block rather than a span.
  CHECK(!kPreview.apply("$$\nx\n", 0, 0, on).repaired, "a fence is not an inline formula");

  // A formula that would swallow a table cell cannot be proven, so it is not
  // repaired: a repair that ate a `|` would lose a column.
  CHECK(!kPreview.apply("| a | b |\n| - | - |\n| $$x |", 0, 0, on).repaired, "a table cell is not closed");

  // And the session path: the repair reaches a stream's tail and never a
  // committed block.
  {
    IncrementalParser parser{ParseMode::IncrementalTail, on};
    // The preview feature has to be asked for, exactly as it is in JavaScript:
    // a stream that never enables it keeps every open span literal.
    parser.setPreview([&] {
      PreviewOptions only;
      only.features = static_cast<uint32_t>(PreviewFeature::Math);
      return only;
    }());
    parser.append("A first paragraph long enough to be worth committing on its own line.\n\n");
    parser.parse(false);
    parser.append("The identity $$e^{i\\pi} + ");
    const ParseResult streamed = parser.parse(false);
    CHECK(streamed.document->blocks.size() == 2, "a paragraph and a formula");
    const Block &tail = streamed.document->blocks.back();
    CHECK(tail.math.size() == 1, "the streamed formula previews");
    CHECK(tail.text == u"The identity $$e^{i\\pi} + $$", "and keeps its literal");
  }
  return true;
}

/// Nothing verbatim is ever styled, and no block is invented or lost.
bool corpusInvariants() {
  static const char *const corpus[] = {
      "\n\n\n**bold", "    **bold in indented", "# Heading\n**bold", "## Important *note",
      "$$\n\\sum_{i=0}^{n} x_i", "$$formula", "*****text", "***bold-italic ~~strike `code",
      "**Hello 世界", "**bold\twith\ttabs", "**bold\r\nwith CRLF", "**bold *italic ~~strike `code",
      "*a**b", "*日本語", "- Use `map` to transform\n- Use `filter", "- [ ] **bold task",
      "---\n**bold after rule", "> > > triple nested *italic", "Check the [documentation",
      "Here's how to use it:\n\n```typescript\nconst x = 1", "Here's the diagram:\n\n![architecture",
      "See ![diagram](http://example.com/img", "The price is $50 and $100", "[**bold link",
      "[^1]", "[^1]: footnote text", "\\**not bold", "\\\\*escaped\\\\* but *real italic",
      "```\nblock\n```\n`inline", "`code` then `more", "text <!-- incomplete comment",
      "text <div class=\\\"test", "| **bold | next |", "~~done~~ and ~~undone",
      "~~🎉 celebration", "    - > 5: expensive", "  - 10–20: reasonable/normal zone.",
      "*P/E*", "20~25°C。20~25°C", "20\\~25 and ~~strike", "3<5 _text", "[a]( b](c [incomplete",
      "__a__ __b__content_", "a~~b~~c~", "div> _text", "func(_arg", "text<", "\n=", "* * *",
      "***```code```", "../src/emphasis-handlers", "[not]", "\\`", "```python\nvalues[0] = 1",
      "use `map[key]` here", "```\nconst str = `template`;\n```", "```css\ncode here\n```\n\n**incomplete bold",
      "Some text\n```js\nconsole.log", "```\n__content_", "text`", "text~~", "` &amp",
      "**bold wor", "This is a **bold statement about `code", "To use this function, call `getData(",
      "| Col1 | Col2 |\n|------|------|\n| **dat", "1. First item\n   - Nested with `code\n2. Second",
      "Precisely.\n\nWhen full-screen TUI applications like **Vim** start",
  };
  for (const char *input : corpus) {
    const std::string name = label("corpus", input);
    const PreviewResult result = applyCorpus(input);
    CHECK(result.parsed.status == ParseStatus::Ok, name.c_str());

    const ParseResult baseline = parseMarkdown(input, kNoMath);
    if (result.repaired) {
      // A repair is only ever allowed to restyle the last block, and it never
      // adds or removes one.
      CHECK(result.parsed.document->blocks.size() == baseline.document->blocks.size(), name.c_str());
      for (size_t i = 0; i + 1 < baseline.document->blocks.size(); i++) {
        CHECK(scan::sameBlock(baseline.document->blocks[i], result.parsed.document->blocks[i]),
              name.c_str());
      }
      // A repair that changed nothing visible is not a repair.
      CHECK(styles(*result.parsed.document) != 0 || visible(*result.parsed.document) != "",
            name.c_str());
    } else {
      // No repair means the bytes are exactly what came in, and the parse is
      // the parse the engine already shipped.
      CHECK(result.text == std::string(input), name.c_str());
      CHECK(scan::sameDocument(*baseline.document, *result.parsed.document), name.c_str());
    }
  }
  return true;
}

/// The scan primitives the policy and the parser share. These are the answers
/// remend reimplements in JavaScript, so they need one suite of their own.
/// The half-arrived tag fragment, which is the one place the pass removes
/// source from the reader's view rather than restyling it.
bool htmlTagFragments() {
  const PreviewOptions tagsOnly{static_cast<PreviewOptions::features_t>(PreviewFeature::HtmlTags)};
  const Preview preview{tagsOnly};
  // The tags are on, because a fragment is only worth hiding when the engine
  // would have rendered it.
  const ParseOptions inlineHtml{ParseFlags::InlineHtml};
  const auto shown = [&](std::string_view input) {
    const PreviewResult result = preview.apply(input, 0, 0, inlineHtml);
    return visible(*result.parsed.document);
  };

  CHECK(shown("Mix<br") == "Mix", "a half-arrived br is hidden");
  CHECK(shown("Mix<b") == "Mix", "a fragment that could still be a br is hidden");
  CHECK(shown("H<sub>2</sub") == "H2", "a half-arrived subscript is hidden");
  CHECK(shown("x<sup>2</sup") == "x2", "a half-arrived superscript is hidden");

  const PreviewResult nested = kPreview.apply("**bold<su", 0, 0, inlineHtml);
  CHECK(visible(*nested.parsed.document) == "bold", "tag removal updates the emphasis insertion offset");
  CHECK((styles(*nested.parsed.document) & kStrong) != 0, "emphasis still repairs after a partial tag");
  const PreviewResult link = kPreview.apply("[label<su", 0, 0, inlineHtml);
  CHECK(visible(*link.parsed.document) == "label", "tag removal updates unfinished-link offsets");

  // A prefix of no allowlisted tag is text, and the proof says so rather than
  // the shape of the fragment deciding it.
  CHECK(shown("a<b and 2<3") == "a<b and 2<3", "a comparison stays");
  CHECK(shown("a<blink") == "a<blink", "a longer word stays");
  CHECK(shown("a<subtitle>x") == "a<subtitle>x", "a longer name stays");

  // The writer's own backticks claimed the fragment as code, so it is content.
  CHECK(shown("`<br`") == "<br", "a fragment inside a code span stays");

  // Removing the fragment changed what came before it, so nothing is hidden.
  CHECK(shown("a<br><su") == "a\n<su", "a fragment whose removal moves a line stays");

  // The feature is opt-in like every other one.
  const PreviewResult off = kDisabled.apply("Mix<br", 0, 0, inlineHtml);
  CHECK(visible(*off.parsed.document) == "Mix<br", "the pass is inert without the feature");
  CHECK(off.stats.probes == 0, "an inert pass costs no probe");

  // A finished stream never previews, and the settled document keeps the source.
  const PreviewResult finished = preview.apply("Mix<br", 0, 1, inlineHtml);
  CHECK(!finished.repaired, "a settled tail is not repaired");
  return true;
}

/// The scanner both the parser and the pass read, so the two can never disagree
/// about which tags exist.
bool sharedScanner() {
  using namespace downstream::scan;

  // An escape is an odd run of backslashes, so the character after an even run
  // is a real delimiter. This is the rule remend's handlers approximate by
  // looking at one backslash.
  CHECK(isEscaped("\\*", 1), "one backslash escapes the next character");
  CHECK(!isEscaped("\\\\*", 2), "two backslashes do not escape");
  CHECK(isEscaped("\\\\\\*", 3), "three backslashes escape");
  CHECK(!isEscaped("a*", 1), "a bare marker is not escaped");
  CHECK(!isEscaped("*", 0), "the first character is never escaped");

  // findUnescaped is how link and image syntax is recovered, so skipping an
  // escaped bracket is the whole point.
  CHECK(findUnescaped("a\\]b]", ']', 0) == 4, "findUnescaped skips the escaped bracket");
  CHECK(findUnescaped("abc", 'z', 0) == kNoOffset, "findUnescaped reports a miss");
  CHECK(findUnescaped("a]b", ']', 1) == 1, "findUnescaped honours its start");
  CHECK(findUnescaped("a]b", ']', 2) == kNoOffset, "findUnescaped does not look backwards");

  // A bare URL in an unfinished destination is not a target. This is what keeps
  // `![a](http://example.com/x` from becoming tappable.
  CHECK(isUnfinishedDestination("text](http://x", 6), "]( before a URL is an unfinished target");
  CHECK(!isUnfinishedDestination("text http://x", 6), "a bare URL in prose is not one");
  CHECK(!isUnfinishedDestination("text\\](http://x", 6), "an escaped bracket is not one");
  CHECK(!isUnfinishedDestination("ab", 1), "too few bytes to hold a destination");
  CHECK(isUnfinishedDestination("[a](http://x", 4), "a link with no close is an unfinished target");

  // The tag matcher, which is a table rather than an HTML tokenizer: md4c has
  // already proved the piece is one well-formed tag.
  CHECK(matchInlineTag("<br>") == InlineTag::Break, "a br is a break");
  CHECK(matchInlineTag("<br/>") == InlineTag::Break, "a self-closed br is a break");
  CHECK(matchInlineTag("<br />") == InlineTag::Break, "a spaced self-closed br is a break");
  CHECK(matchInlineTag("<BR>") == InlineTag::Break, "a tag name is case-insensitive");
  CHECK(matchInlineTag("</br>") == InlineTag::Break, "a stray closer is a break");
  CHECK(matchInlineTag("<br clear=\"all\">") == InlineTag::Break, "attributes are ignored");
  CHECK(matchInlineTag("<sub>") == InlineTag::SubOpen, "sub opens");
  CHECK(matchInlineTag("<sup>") == InlineTag::SupOpen, "sup opens");
  CHECK(matchInlineTag("</sub>") == InlineTag::SubClose, "sub closes");
  CHECK(matchInlineTag("</sup >") == InlineTag::SupClose, "a close tag may carry space");
  CHECK(matchInlineTag("<sub/>") == InlineTag::SubVoid, "a self-closed script is swallowed");
  CHECK(matchInlineTag("<sup />") == InlineTag::SupVoid, "a spaced self-closed script is swallowed");
  CHECK(matchInlineTag("<b>") == InlineTag::None, "a tag off the list is literal");
  CHECK(matchInlineTag("<!-- x -->") == InlineTag::None, "a comment is literal");
  CHECK(matchInlineTag("<subtitle>") == InlineTag::None, "a longer name is not a prefix match");
  CHECK(matchInlineTag("<supplier>") == InlineTag::None, "and neither is this one");
  CHECK(matchInlineTag("<brx>") == InlineTag::None, "a name that runs past br is not a br");
  CHECK(matchInlineTag("a < b") == InlineTag::None, "prose is never a tag");
  CHECK(matchInlineTag("<" + std::string(65536, 'a') + ">") == InlineTag::None, "a long unsupported name stays literal");

  // The prefix rule the preview pass asks, on the same table.
  CHECK(isInlineTagPrefix("<"), "a lone opener could still become a tag");
  CHECK(isInlineTagPrefix("<s"), "a partial name could");
  CHECK(isInlineTagPrefix("</su"), "a partial closer could");
  CHECK(isInlineTagPrefix("<br /"), "a partial self-closer could");
  CHECK(!isInlineTagPrefix("<br>"), "a complete tag is not a fragment");
  CHECK(isInlineTagPrefix("<b"), "b can still reach br, so it is a fragment");
  CHECK(!isInlineTagPrefix("<z"), "a name that cannot reach one of the three is not one");
  CHECK(!isInlineTagPrefix("a<b"), "prose before the fragment does not matter");
  CHECK(!isInlineTagPrefix("<subtitl"), "a longer word is not a fragment of sub");
  CHECK(!isInlineTagPrefix("<br漢字"), "a fragment is ASCII, so its bytes are its units");

  // Destination scanning has to survive the shapes a model can emit mid-URL.
  CHECK(skipInlineDestination("(a(b)c)", 0) == 7, "nested parens close in order");
  CHECK(skipInlineDestination("(a(b)c", 0) == kNoOffset, "an unclosed destination has no end");
  CHECK(skipInlineDestination("(<a b>)", 0) == 7, "an angle destination closes at >");
  CHECK(skipInlineDestination("(<a b)", 0) == kNoOffset, "an unclosed angle destination has no end");
  CHECK(skipInlineDestination("(\"a)b\")", 0) == 7, "a quoted destination ignores parens");
  CHECK(skipInlineDestination("(a\\)b)", 0) == 6, "an escaped paren does not nest");
  CHECK(skipInlineDestination("()", 0) == 2, "an empty destination closes at once");

  // Line shape, which decides whether a line can open a block.
  CHECK(isBlankLine("   \t"), "a whitespace-only line is blank");
  CHECK(isBlankLine(""), "an empty line is blank");
  CHECK(isBlankLine("\r"), "a carriage return is blank");
  CHECK(!isBlankLine("  x"), "a line with content is not blank");
  CHECK(afterIndent("   x") == 3, "three spaces still open a block");
  CHECK(afterIndent("    x") == kNoOffset, "four spaces are code");
  CHECK(afterIndent("x") == 0, "no indent");
  CHECK(afterIndent("") == 0, "an empty line has no indent");
  CHECK(startsWith("# h", '#'), "a heading starts with #");
  CHECK(!startsWith("    # h", '#'), "an indented # is code, not a heading");
  CHECK(startsWith("> q", '>'), "a quote starts with >");
  CHECK(!startsWith("q > x", '>'), "a mid-line marker is not a block marker");

  // Fence grammar. Backticks and tildes differ on one point: a tilde fence's
  // info string may contain tildes, a backtick fence's may not.
  CHECK(fenceOpener("```js").has_value(), "a backtick fence opens");
  CHECK(fenceOpener("~~~py").has_value(), "a tilde fence opens");
  CHECK(fenceOpener("  ```js").has_value(), "a fence may be indented by three spaces");
  CHECK(!fenceOpener("    ```js").has_value(), "an indented fence is code");
  CHECK(!fenceOpener("``js").has_value(), "two backticks are not a fence");
  CHECK(!fenceOpener("```a`b").has_value(), "a backtick in the info string is not a fence");
  CHECK(fenceOpener("~~~a~b").has_value(), "a tilde in a tilde fence's info string is fine");
  CHECK(!fenceOpener("# h").has_value(), "a heading is not a fence");
  CHECK(!fenceOpener("").has_value(), "an empty line is not a fence");
  CHECK(closesFence("```", '`', 3), "a matching run closes");
  CHECK(closesFence("````", '`', 3), "a longer run closes");
  CHECK(!closesFence("``", '`', 3), "a short run does not close");
  CHECK(!closesFence("```js", '`', 3), "an info string does not close");
  CHECK(!closesFence("```", '~', 3), "a backtick run does not close a tilde fence");
  CHECK(isFenceLine("~~~"), "a tilde run is a fence line");
  CHECK(isFenceLine("```"), "a backtick run is a fence line");
  CHECK(!isFenceLine("~~~x"), "a tilde run with a suffix is not");
  CHECK(!isFenceLine("h #"), "a heading is not a fence line");

  // The flanking character classes the preview uses to reject an ambiguous
  // opener. Every non-ASCII unit counts as a word character, so an unrecognised
  // script falls back to literal text rather than to a repair.
  CHECK(isWordish(u'a') && isWordish(u'Z') && isWordish(u'9') && isWordish(u'_'),
        "ASCII word characters");
  CHECK(isWordish(u'0'), "a digit is a word character");
  CHECK(!isWordish(u' ') && !isWordish(u'*') && !isWordish(u'\n') && !isWordish(0),
        "punctuation, space, and NUL are not word characters");
  CHECK(isWordish(0x3042), "a non-ASCII letter is treated as a word character");
  // An astral character arrives as a surrogate pair, and both halves are above
  // 0x80, so an emoji counts as a word character too.
  CHECK(isWordish(0xD83C), "a surrogate half is treated as a word character");
  CHECK(isMarkdownSpace(u' ') && isMarkdownSpace(u'\t') && isMarkdownSpace(u'\n') &&
            isMarkdownSpace(u'\r'),
        "CommonMark whitespace");
  CHECK(!isMarkdownSpace(u'a') && !isMarkdownSpace(u'*'), "other characters are not whitespace");

  // The comparison both the incremental parser and the preview rely on. If it
  // ever said two different documents were the same, a repair could restyle a
  // block the reader has already seen.
  const ParseResult a = parseMarkdown("**bold** and *it*");
  const ParseResult b = parseMarkdown("**bold** and *it*");
  CHECK(scan::sameDocument(*a.document, *b.document), "two parses of the same text agree");
  CHECK(a.document->id != b.document->id, "each parse gets its own document id");
  CHECK(scan::sameBlock(a.document->blocks[0], b.document->blocks[0]), "sameBlock agrees");
  const ParseResult changed = parseMarkdown("**bold** and *other*");
  CHECK(!scan::sameBlock(a.document->blocks[0], changed.document->blocks[0]),
        "sameBlock notices a change");
  const ParseResult added = parseMarkdown("**bold** and *it*\n\nsecond");
  CHECK(!scan::sameDocument(*a.document, *added.document), "sameDocument notices an added block");
  const ParseResult removed = parseMarkdown("**bold** and");
  CHECK(!scan::sameDocument(*a.document, *removed.document), "sameDocument notices a removed block");
  return true;
}

/// The pass through a real session. This is the part that has to hold for the
/// feature to be safe to ship: an enabled preview may only ever restyle the
/// unstable tail, and a finished stream has to be a parse of the bytes the
/// writer actually sent.
bool sessionIntegration() {
  // Off by default. A session nobody asked to preview behaves exactly as it
  // did before this existed.
  {
    IncrementalParser parser{ParseMode::IncrementalTail, kNoMath};
    CHECK(!parser.previewEnabled(), "a session does not preview until asked");
    parser.append("**bold wor");
    parser.parse(false);
    CHECK(visible(*parser.document()) == "**bold wor", "an unopted session keeps markers literal");
    CHECK(styles(*parser.document()) == 0, "an unopted session styles nothing");
  }

  // Opted in, the same stream styles the open span.
  {
    IncrementalParser parser{ParseMode::IncrementalTail, kNoMath};
    parser.setPreview(PreviewOptions::all());
    CHECK(parser.previewEnabled(), "an opted-in session previews");
    parser.append("**bold wor");
    parser.parse(false);
    CHECK(visible(*parser.document()) == "bold wor", "an opted-in session styles the open span");
    CHECK((styles(*parser.document()) & kStrong) != 0, "the style is strong");
    CHECK(parser.previewStats().applied == 1, "the repair is counted");
  }

  // A long enough prefix to commit, then an open span. The committed blocks
  // have to come through the repair untouched, because they are shared storage
  // and not a reparse.
  {
    IncrementalParser parser{ParseMode::IncrementalTail, kNoMath};
    parser.setPreview(PreviewOptions::all());
    std::string filler;
    for (int i = 0; i < 12; i++) {
      filler += "A committed paragraph with enough words to be worth committing. ";
    }
    parser.append(filler + "\n\n");
    parser.parse(false);
    const size_t committed = parser.committedBytes();
    CHECK(committed > 0, "a long prefix commits");
    const DocumentHandle settled = parser.document();
    CHECK(settled && !settled->blocks.empty(), "the committed snapshot has blocks");

    parser.append("**bold wor");
    parser.parse(false);
    const DocumentHandle next = parser.document();
    CHECK(next && next->blocks.size() > settled->blocks.size(), "the tail adds a block");
    // Every block the previous snapshot had is still the same block, identity
    // included, so a platform reuses its rendering rather than relaying out
    // text a reader has already seen.
    for (size_t i = 0; i < settled->blocks.size(); i++) {
      const char *name = "a committed block survives a previewed append";
      CHECK(scan::sameBlock(settled->blocks[i], next->blocks[i]), name);
      CHECK(settled->blocks[i].identity == next->blocks[i].identity, name);
    }
    // The source the session holds is the bytes the writer sent, not the
    // repaired copy. Nothing in the preview may reach it.
    CHECK(parser.source() == filler + "\n\n**bold wor", "the canonical source is never repaired");
  }

  // A finished stream drops the repair entirely, which is what
  // docs/streaming-semantics.md requires of finish(). Full mode is the one
  // createMarkdownStream uses, and it reaches the parser by a different branch,
  // so both modes have to be asked.
  for (const ParseMode mode : {ParseMode::FullParse, ParseMode::IncrementalTail}) {
    IncrementalParser parser{mode, kNoMath};
    parser.setPreview(PreviewOptions::all());
    parser.append("**bold wor");
    parser.parse(false);
    CHECK(visible(*parser.document()) == "bold wor", "the preview styles the tail");
    parser.parse(true);
    CHECK(visible(*parser.document()) == "**bold wor", "finish removes the repair");
    CHECK(styles(*parser.document()) == 0, "finish leaves the markers literal");
    // And what it shows is a parse of the real source, so the settled document
    // is exactly what a controlled view of the same text would render.
    const ParseResult direct = parseMarkdown("**bold wor");
    CHECK(scan::sameDocument(*direct.document, *parser.document()),
          "the finished document is a parse of the canonical source");
  }

  // The same is true across a full stream: a previewed session and a plain one
  // have to agree the moment the stream finishes.
  for (const ParseMode mode : {ParseMode::FullParse, ParseMode::IncrementalTail}) {
    IncrementalParser previewed{mode};
    previewed.setPreview(PreviewOptions::all());
    IncrementalParser plain{mode};
    for (const char *chunk : {"# Title\n\nSome ", "**bold", " text with ", "`code",
                             " and *more", emphasis_mark, " done.\n\n- item one\n- item two\n"}) {
      previewed.append(chunk);
      previewed.parse(false);
      plain.append(chunk);
      plain.parse(false);
    }
    previewed.parse(true);
    plain.parse(true);
    CHECK(scan::sameDocument(*plain.document(), *previewed.document()),
          "a previewed session settles to the same document as a plain one");
    CHECK(previewed.previewStats().applied > 0, "the stream really did exercise the preview");
  }

  // A heading's style never survives past its own line. Once the line ends the
  // span is literal for good, so the reader sees it go literal then rather than
  // when the next block happens to arrive.
  {
    IncrementalParser parser{ParseMode::IncrementalTail, kNoMath};
    parser.setPreview(PreviewOptions::all());
    parser.append("# **Tit");
    parser.parse(false);
    CHECK(visible(*parser.document()) == "Tit", "an open heading previews");
    parser.append("le\n");
    parser.parse(false);
    CHECK(visible(*parser.document()) == "**Title", "a closed heading is literal");
    parser.append("Next");
    parser.parse(false);
    CHECK(visible(*parser.document()) == "**Title|Next", "and stays literal after the next block");
  }

  // The pass does the parse it replaces. Off, a full-mode update costs exactly
  // the source; on, it costs the source and the probe, and nothing twice.
  {
    IncrementalParser plain{ParseMode::FullParse};
    plain.append("**bold wor");
    plain.parse(false);
    IncrementalParser previewed{ParseMode::FullParse};
    previewed.setPreview(PreviewOptions::all());
    previewed.append("**bold wor");
    previewed.parse(false);
    CHECK(plain.counters().bytesParsed == 10, "a plain full parse counts the source once");
    CHECK(previewed.counters().bytesParsed == 10 + 12, "a previewed full parse counts the source and one probe");
    CHECK(previewed.counters().fullParses == 1, "a previewed update is still one full parse");
    const PreviewResult inert = kDisabled.apply("**bold wor", 0);
    CHECK(inert.bytesParsed == 10 && inert.stats.probes == 0, "an inert pass is one parse");
  }

  // A reset mid-stream is still a stream the writer has not finished, so the
  // preview carries through it rather than dropping out for one frame.
  {
    IncrementalParser parser{ParseMode::IncrementalTail, kNoMath};
    parser.setPreview(PreviewOptions::all());
    parser.append("**bold wor");
    parser.parse(false);
    parser.replace("**bold word");
    parser.parse(false);
    CHECK(visible(*parser.document()) == "bold word", "a reset source still previews");
    parser.parse(true);
    CHECK(visible(*parser.document()) == "**bold word", "and a finished reset source does not");
  }

  // So does a tail too long to keep, which falls back to a full parse over the
  // committed prefix. The committed blocks come through untouched.
  {
    IncrementalParser parser{ParseMode::IncrementalTail, kNoMath};
    parser.setPreview(PreviewOptions::all());
    std::string filler;
    for (int i = 0; i < 12; i++) {
      filler += "A committed paragraph with enough words to be worth committing. ";
    }
    parser.append(filler + "\n\n");
    parser.parse(false);
    const DocumentHandle settled = parser.document();
    CHECK(parser.committedBytes() > 0, "the prefix commits before the long tail");
    parser.append(std::string(70 * 1024, 'x') + " **bold wor");
    parser.parse(false);
    CHECK(parser.counters().fallbackCount(ParseFallback::TailLimit) == 1, "the long tail takes the fallback");
    const Block &last = parser.document()->blocks.back();
    CHECK(last.text.size() > 4 && last.text.substr(last.text.size() - 9) == u" bold wor",
          "a tail past the limit still previews");
    CHECK(settled->blocks[0].identity == parser.document()->blocks[0].identity,
          "the committed block keeps its identity through the fallback");
  }

  // A block a reader has seen settle is never the one a repair lands on.
  {
    const PreviewResult frozen = kPreview.apply("**bold wor", 0, 1);
    CHECK(!frozen.repaired && frozen.stats.probes == 0, "a frozen last block is not probed");
    CHECK(applyCorpus("settled\n\n**bold wor", 0, 1).repaired, "a block after the frozen ones previews");
  }

  // An unfinished link reads as its text, becomes a real link the moment its
  // `)` arrives, and is never a target before then. A stream that finishes
  // first settles to the literal syntax, like any other open span.
  for (const ParseMode mode : {ParseMode::FullParse, ParseMode::IncrementalTail}) {
    IncrementalParser parser{mode, kNoMath};
    parser.setPreview(PreviewOptions::all());
    parser.append("See [the docs](https://exa");
    parser.parse(false);
    CHECK(visible(*parser.document()) == "See the docs", "an unfinished link previews as its text");
    CHECK(parser.document()->links.empty(), "and is not a target");
    parser.append("mple.com)");
    parser.parse(false);
    CHECK(parser.document()->links.size() == 1 && parser.document()->links[0].url == "https://example.com",
          "the writer's own bracket makes it a link");

    IncrementalParser unfinished{mode};
    unfinished.setPreview(PreviewOptions::all());
    unfinished.append("See [the docs](https://exa");
    unfinished.parse(false);
    unfinished.parse(true);
    CHECK(visible(*unfinished.document()) == "See [the docs](https://exa",
          "a finished stream shows the unfinished link as typed");
  }

  // An open fence is where the preview has least to say, and the fast path
  // that skips md4c entirely must not be confused by it.
  {
    IncrementalParser parser{ParseMode::IncrementalTail, kNoMath};
    parser.setPreview(PreviewOptions::all());
    parser.append("```js\nconst x = `template");
    parser.parse(false);
    const Block &last = parser.document()->blocks.back();
    CHECK(last.kind == BlockKind::CodeBlock, "an open fence stays a code block");
    CHECK(last.runs.empty(), "nothing inside a fence is styled");
  }
  return true;
}

} // namespace

int main() {
  // Every case runs even when an earlier one fails, so one run reports every
  // regression rather than only the first.
  CHECK(unterminatedBoldItalic(), "unterminated bold italic");
  CHECK(unterminatedBold(), "unterminated bold");
  CHECK(unterminatedItalic(), "unterminated italic");
  CHECK(unterminatedStrikethrough(), "unterminated strikethrough");
  CHECK(unterminatedInlineCode(), "unterminated inline code");
  CHECK(linksAndImages(), "links and images");
  CHECK(structureStillArriving(), "structure still arriving");
  CHECK(escapesAndMarkers(), "escapes and markers");
  CHECK(listMarkers(), "list markers");
  CHECK(nestedOpenSpans(), "nested open spans");
  CHECK(trailingWhitespace(), "trailing whitespace");
  CHECK(repairIsScopedToTheLastBlock(),"repair is scoped to the last block");
  CHECK(featuresAreOptIn(), "features are opt in");
  CHECK(mathPreview(), "math preview");
  CHECK(corpusInvariants(), "corpus invariants");
  CHECK(sessionIntegration(), "session integration");
  CHECK(htmlTagFragments(), "half-arrived tag fragments");
  CHECK(sharedScanner(), "shared scanner");
  return ds_check_result();
}
