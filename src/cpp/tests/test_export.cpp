// Engine checks for the copy and download controls that no other host binary
// covers: the segment plan a code card depends on, and the export formats and
// file names every platform writes from.
//
//     node --test src/cpp/tests/engine.test.mjs   (runs test_export)

#include "CodeBlocks.h"
#include "CodeHighlighter.h"
#include "CodeLayout.h"
#include "Controls.h"
#include "Export.h"
#include "InlineRuns.h"
#include "MarkdownParser.h"
#include "SegmentPlan.h"
#include "BidiTable.h"
#include "DetailsLayout.h"
#include "Direction.h"
#include "TagSlots.h"
#include "TableLayout.h"
#include "Unicode.h"
#include "tests/check.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

using namespace downstream;

namespace {

DocumentHandle parse(std::string_view source) {
  ParseResult result = parseMarkdown(source);
  if (result.status != ParseStatus::Ok) {
    return nullptr;
  }
  return result.document;
}

/// UTF-8, so a golden with an emoji or a replacement character reads as it is
/// written rather than as a placeholder.
std::string narrow(const std::u16string &text) {
  return utf16ToUtf8(text);
}

void expectEq(const std::string &actual, std::string_view expected, const char *label) {
  if (actual != expected) {
    std::fprintf(stderr, "FAIL %s\n  expected: %s\n  actual:   %s\n", label, std::string(expected).c_str(),
                 actual.c_str());
    ds_check_failures++;
  }
}

/// A golden is written as its own bytes. Some Markdown exports cannot be one
/// readable literal: a code span holding a backtick is spelled with the same
/// characters a reader would read as something else, so those cases build the
/// expectation out of pieces and say why.
void check(std::string_view expected, const char *label, const std::string &actual) {
  expectEq(actual, expected, label);
}

void checkPlan(const char *label, std::string_view source, const char *expected) {
  const DocumentHandle document = parse(source);
  if (!document) {
    CHECK(false, label);
    return;
  }
  std::string shape;
  for (const SegmentPlan &segment : planSegments(*document)) {
    if (!shape.empty()) {
      shape.push_back(' ');
    }
    // One letter per kind rather than a table flag and a code flag, because the
    // plan has six kinds and the last three were added after the other three: a
    // boolean per kind is how a new one gets drawn as prose and measured as text.
    switch (segment.kind) {
      case SegmentKind::Table: shape += 'T'; break;
      case SegmentKind::Code: shape += 'C'; break;
      case SegmentKind::Math: shape += 'M'; break;
      case SegmentKind::Image: shape += 'I'; break;
      case SegmentKind::Text: shape += 't'; break;
      case SegmentKind::Custom: shape += 'U'; break;
    }
    shape += std::to_string(segment.from);
    shape.push_back('-');
    shape += std::to_string(segment.to);
  }
  expectEq(shape, expected, label);
}

/// Serializes the first `blocks` of a document, or all of them when `blocks` is
/// zero. A document with a footnote definition after its table has to name how
/// many blocks the table is.
std::string exportOne(std::string_view source, ExportFormat format, char16_t separator = u',', size_t blocks = 0) {
  const DocumentHandle document = parse(source);
  if (!document) {
    return "<no document>";
  }
  const size_t to = blocks == 0 ? document->blocks.size() : std::min(blocks, document->blocks.size());
  return narrow(exportBlocks(*document, 0, to, format, separator));
}

void checkExport(std::string_view expected, const char *label, std::string_view source, ExportFormat format,
                 char16_t separator = u',', size_t blocks = 0) {
  check(expected, label, exportOne(source, format, separator, blocks));
}

void checkName(const char *label, const char *expected, std::string_view info, std::string_view language,
               std::string_view configured = "", std::string_view fallback = "file") {
  check(expected, label,
        downloadName(parseFenceMeta(info), language, std::string(configured), std::string(fallback)));
}

// ------------------------------------------------------------- segment plan

void testSegmentPlan() {
  checkPlan("prose only is one text segment", "one\n\ntwo\n", "t0-2");
  checkPlan("a table is its own segment", "a\n\n| x | y |\n| - | - |\n| 1 | 2 |\n", "t0-1 T1-3");
  checkPlan("a code block is its own segment", "a\n\n```js\nx\n```\n", "t0-1 C1-2");
  checkPlan("code first and last", "```js\nx\n```\n\nb\n\n```js\ny\n```\n", "C0-1 t1-2 C2-3");
  checkPlan("two code blocks never share a segment", "```\na\n```\n\n```\nb\n```\n", "C0-1 C1-2");
  checkPlan("indented code is a code segment too", "a\n\n    x\n", "t0-1 C1-2");
  checkPlan("a code block inside a list is one segment", "- item\n\n  ```js\n  x\n  ```\n", "t0-1 C1-2");
  checkPlan("a code block inside a quote is one segment", "> q\n>\n> ```js\n> x\n> ```\n", "t0-1 C1-2");
  checkPlan("a code block inside a list inside a quote is one segment", "> - item\n>\n>   ```js\n>   x\n>   ```\n",
            "t0-1 C1-2");
  checkPlan("a rule is not a code segment", "a\n\n---\n\nb\n", "t0-3");
  checkPlan("a table and a code block in one document", "| x |\n| - |\n| 1 |\n\n```js\nx\n```\n", "T0-2 C2-3");

  // A formula is a segment of its own for the same reason a code block is: it has
  // its own box, its own theme slots, and its own height, so sharing a segment
  // with prose would mean measuring it as text and drawing it as neither.
  checkPlan("a lone display formula is its own segment", "a\n\n$$x^2$$\n\nb\n", "t0-1 M1-2 t2-3");
  checkPlan("a display formula first", "$$x$$\n\nb\n", "M0-1 t1-2");
  checkPlan("a math fence is a math segment", "```math\nx\n```\n\nb\n", "M0-1 t1-2");
  checkPlan("a display formula and a code block in one document", "$$x$$\n\n```js\nx\n```\n", "M0-1 C1-2");

  // Inline math stays inside a text segment: it is the height of the line it sits
  // in, not a box of its own.
  checkPlan("inline math stays in prose", "a $$x$$ b\n", "t0-1");

  // A picture-only paragraph is its own segment, and its ordinal is the picture's
  // index in Document::images, which is what a view matches a state entry against.
  checkPlan("a picture-only paragraph is its own segment", "a\n\n![alt](https://example.com/a.png)\n\nb\n",
            "t0-1 I1-2 t2-3");
  checkPlan("a picture with prose around it stays prose", "see ![alt](https://example.com/a.png) here\n", "t0-1");
  checkPlan("two pictures on consecutive lines are one image block", "![a](https://example.com/a.png)\n![b](https://example.com/b.png)\n",
            "I0-1");
  checkPlan("a picture and a table in one document", "![a](https://example.com/a.png)\n\n| x |\n| - |\n| 1 |\n",
            "I0-1 T1-3");
  checkPlan("an empty document has no segments", "", "");
}

// ------------------------------------------------------------- wrap choices

/// A block's layout is one membership test against the toggled set, and both
/// platforms resolve it through this one rule. The cases below are the whole
/// table, so a platform's mirror of it can be checked against them.
/// One block's formula, as `tex|resolved|display`, so a document read and the plan
/// read can both be asserted on without a second dump format.
std::string mathShape(std::string_view source) {
  const DocumentHandle document = parse(source);
  if (!document) {
    return "(unparsed)";
  }
  std::string shape;
  for (const Block &block : document->blocks) {
    if (!shape.empty()) {
      shape.push_back(' ');
    }
    shape += std::to_string(static_cast<int>(block.kind));
    for (const MathSpan &span : block.math) {
      shape += "[" + span.tex + "|" + (span.layout ? "1" : "0") + "|" + (span.display ? "1" : "0") + "]";
    }
  }
  return shape;
}

void testMathDocuments() {
  // What a document holds, as opposed to what the plan does with it: the plan
  // passing is necessary and not sufficient, because a formula that never became a
  // span cannot be promoted no matter how the plan walks the blocks.
  check("6[x|0|1]", "a lone $$x$$ is a math block", mathShape("$$x$$\n"));
  check("0[x|0|1]", "a formula with prose stays prose", mathShape("a $$x$$ b\n"));
  check("6[x^2|0|1]", "a lone $$x^2$$ is a math block", mathShape("$$x^2$$\n"));
  check("6[x|0|1]", "a $$ line fence is a math block", mathShape("$$\nx\n$$\n"));
  check("6[x|0|1]", "a ```math fence is a math block", mathShape("```math\nx\n```\n"));
  check("0", "single dollar stays literal by default", mathShape("$x$\n"));
  check("6[x|0|1]", "brackets are a display formula by default", mathShape("\\[x\\]\n"));
  check("0[x|0|0]", "brackets are inline inside prose", mathShape("a \\(x\\) b\n"));

  // Math off is the byte-for-byte old parse, which is the promise the flags make.
  {
    const ParseResult off = parseMarkdown("$$x$$\n", ParseOptions{0});
    CHECK(off.status == ParseStatus::Ok && off.document, "a document with math off still parses");
    if (off.document) {
      CHECK(off.document->blocks.size() == 1, "math off leaves one block");
      CHECK(off.document->blocks[0].kind == BlockKind::Paragraph, "math off leaves a formula a paragraph");
      CHECK(off.document->blocks[0].math.empty(), "math off records no span");
    }
  }
}

void testWrapChoices() {
  const std::vector<uint32_t> none;
  const std::vector<uint32_t> third{2};
  CodeLayout unwrapped;
  CodeLayout wrapped;
  wrapped.props.wrap = true;

  auto layoutOf = [](const CodeLayout &layout, uint32_t ordinal) {
    return resolveCodeBlock(layout, ordinal, FenceMeta{}).wraps;
  };

  // Nothing toggled: every block follows the prop.
  CHECK(layoutOf(unwrapped, 0) == false, "an untoggled block does not wrap when the prop is off");
  CHECK(layoutOf(wrapped, 0) == true, "an untoggled block wraps when the prop is on");

  // One block toggled: it is the only one that differs from the prop, and the
  // rest still follow it. This is the case a single chrome bit cannot express.
  CodeLayout mixedUnwrapped = unwrapped;
  mixedUnwrapped.choices.wrapToggles = third;
  CodeLayout mixedWrapped = wrapped;
  mixedWrapped.choices.wrapToggles = third;
  CHECK(layoutOf(mixedUnwrapped, 2) == true, "a toggled block wraps when the prop is off");
  CHECK(layoutOf(mixedWrapped, 2) == false, "a toggled block stops wrapping when the prop is on");
  CHECK(layoutOf(mixedUnwrapped, 0) == false, "an untoggled neighbour still follows the prop being off");
  CHECK(layoutOf(mixedWrapped, 0) == true, "an untoggled neighbour still follows the prop being on");
  CHECK(layoutOf(mixedUnwrapped, 1) == false, "a neighbour before the toggle is unaffected");
  CHECK(layoutOf(mixedUnwrapped, 3) == false, "a neighbour after the toggle is unaffected");

  // The chrome bit says only whether every block wraps, and the toggle set is
  // what fills in a mixed document.
  CHECK((chromeForProps(ControlBits::CodeWrap, CodeLayoutProps{true, true, 400}, ReaderChoices{}) & kChromeCodeWrap) != 0,
        "no toggles with the prop on is all-wrapping chrome");
  CHECK((chromeForProps(ControlBits::CodeWrap, CodeLayoutProps{true, true, 400}, mixedWrapped.choices) & kChromeCodeWrap) == 0,
        "any toggle clears the all-wrapping bit");
  CHECK((chromeForProps(ControlBits::CodeWrap, CodeLayoutProps{}, mixedUnwrapped.choices) & kChromeCodeWrap) == 0,
        "the prop off is never all-wrapping chrome");

  // The header and the toolbar do not depend on any of this.
  CHECK((chromeForProps(ControlBits::CodeCopy, CodeLayoutProps{}, mixedUnwrapped.choices) & kChromeCodeHeader) != 0,
        "a copy control still gives a code block its header");
  CHECK((chromeForProps(ControlBits::CodeExpand, CodeLayoutProps{}, ReaderChoices{}) & kChromeCodeHeader) != 0,
        "an expand control alone still gives a code block its header");
  CHECK((chromeForProps(ControlBits::TableDownload, CodeLayoutProps{}, ReaderChoices{}) & kChromeTableToolbar) != 0,
        "a table download still gives a table its toolbar");
  CHECK((chromeForProps(0, CodeLayoutProps{}, ReaderChoices{}) & (kChromeCodeHeader | kChromeTableToolbar)) == 0,
        "no controls means no chrome at all");

  // The wrap control being off is a different thing from the prop being off: the
  // button disappears but the layout the prop asked for stays.
  CHECK((chromeForProps(0, CodeLayoutProps{true, true, 400}, ReaderChoices{}) & kChromeCodeWrap) == 0, "no wrap control means no wrap bit");
}

// ----------------------------------------------------------- line numbers

/// Every combination of the prop, the fence's meta, and the reader's choices,
/// which is the whole table `resolveCodeBlock` has to answer.
void testCodeBlockLayout() {
  const FenceMeta plain;
  FenceMeta hidden;
  hidden.noLineNumbers = true;
  FenceMeta tenth;
  tenth.startLine = 10;

  CodeLayout layout;
  CodeLayout noNumbers = layout;
  noNumbers.props.lineNumbers = false;
  CodeLayout capped = layout;
  capped.props.maxHeight = 400;

  auto numbers = [](const CodeLayout &value, uint32_t ordinal, const FenceMeta &meta) {
    return resolveCodeBlock(value, ordinal, meta);
  };

  CHECK(numbers(layout, 0, plain).lineNumbers, "numbers are on by default");
  CHECK(!numbers(noNumbers, 0, plain).lineNumbers, "the prop turns numbers off");
  // The prop wins over the meta, as Streamdown's `lineNumbers={false}` does over
  // its own meta string.
  CHECK(!numbers(noNumbers, 0, FenceMeta{}).lineNumbers, "the prop wins over a fence that asked for numbers");
  CHECK(!numbers(layout, 0, hidden).lineNumbers, "noLineNumbers hides them for one block");
  CHECK(numbers(layout, 1, plain).lineNumbers, "one block's meta does not affect its neighbour");

  CHECK(numbers(layout, 0, tenth).firstNumber == 10, "the fence's start line reaches the gutter");
  CHECK(numbers(layout, 0, plain).firstNumber == 1, "no start line means line one");

  // The cap, and the expand choice that removes it for one block.
  CHECK(numbers(capped, 0, plain).maxHeight == 400.0f, "the cap prop reaches the block");
  CodeLayout expanded = capped;
  expanded.choices.codeExpanded = {1};
  CHECK(numbers(expanded, 1, plain).maxHeight == 0.0f, "an expanded block has no cap");
  CHECK(numbers(expanded, 0, plain).maxHeight == 400.0f, "one expanded block does not cap its neighbour");
  CHECK(numbers(capped, 0, plain).maxHeight == 400.0f, "an uncapped layout leaves the cap alone");
  CHECK(numbers(expanded, 1, plain).expanded, "an expanded block says so, so it can collapse again");
  CHECK(!numbers(expanded, 0, plain).expanded, "its neighbour is not expanded");

  // A prop of 0 is no cap, and it must reach the block as the same 0 an
  // expanded block uses rather than as a truthy "off".
  CodeLayout uncapped = layout;
  uncapped.props.maxHeight = 0;
  CHECK(numbers(uncapped, 0, plain).maxHeight == 0.0f, "a zero cap is no cap");
  CHECK(numbers(uncapped, 0, plain).maxHeight == numbers(expanded, 1, plain).maxHeight,
        "uncapped and expanded are the same value to the card");
  // With no cap there is nothing to collapse back to, so a leftover choice does
  // not turn into a collapse button.
  CodeLayout staleExpand = uncapped;
  staleExpand.choices.codeExpanded = {0};
  CHECK(!numbers(staleExpand, 0, plain).expanded, "a block with no cap is never expanded");
}

/// The table's cap and the reader's per-table expand choice.
///
/// The same rules `resolveCodeBlock` follows for a code block, deliberately: a
/// table and a code card are the same decision about a different block, and one
/// definition of "the reader lifted the cap" is what keeps them from drifting.
/// The ordinals are separate sets, because a code ordinal counts code blocks and
/// a table ordinal counts tables.
void testTableLayout() {
  TableLayout layout;
  TableLayout capped = layout;
  capped.props.maxHeight = 300;

  auto tables = [](const TableLayout &value, uint32_t ordinal) { return resolveTable(value, ordinal); };

  CHECK(tables(capped, 0).maxHeight == 300.0f, "the cap prop reaches the table");
  CHECK(!tables(capped, 0).expanded, "no choice means no expansion");

  TableLayout expanded = capped;
  expanded.choices.tableExpanded = {1};
  CHECK(tables(expanded, 1).maxHeight == 0.0f, "an expanded table has no cap");
  CHECK(tables(expanded, 1).expanded, "an expanded table says so, so it can collapse again");
  CHECK(tables(expanded, 0).maxHeight == 300.0f, "one expanded table does not cap its neighbour");
  CHECK(!tables(expanded, 0).expanded, "its neighbour is not expanded");

  // A prop of 0 is no cap, and it must reach the table as the same 0 an expanded
  // table uses rather than as a truthy "off".
  TableLayout uncapped = layout;
  uncapped.props.maxHeight = 0;
  CHECK(tables(uncapped, 0).maxHeight == 0.0f, "a zero cap is no cap");
  CHECK(tables(uncapped, 0).maxHeight == tables(expanded, 1).maxHeight,
        "uncapped and expanded are the same value to the table");
  // With no cap there is nothing to collapse back to, so a leftover choice does
  // not turn into a collapse button.
  TableLayout staleExpand = uncapped;
  staleExpand.choices.tableExpanded = {0};
  CHECK(!tables(staleExpand, 0).expanded, "a table with no cap is never expanded");

  // The two caps read different sets of the reader's one choice bundle, so
  // expanding a code block never uncaps a table that happens to share its
  // ordinal. This is the property that lets one set of choices replace two.
  TableLayout codeOnly = capped;
  codeOnly.choices.codeExpanded = {1};
  CHECK(!tables(codeOnly, 1).expanded, "a code expand does not uncap a table");
  CHECK(resolveCodeBlock(CodeLayout{CodeLayoutProps{true, true, 400}, codeOnly.choices}, 1, FenceMeta{}).expanded,
        "and the code block is the one that expands");

  // A negative ordinal is a caller that does not know which table this is, and
  // the answer is the same one an uncapped table gets: no cap, and nothing to
  // collapse. Reading it as a huge unsigned ordinal would silently apply the cap
  // to a table the reader never chose.
  CHECK(tables(capped, -1).maxHeight == 0.0f, "an unknown table has no cap");
  CHECK(!tables(expanded, -1).expanded, "an unknown table is never expanded");

  // A NaN cap must not make two identical props compare different, which is the
  // same rule `TableLayoutProps` documents and for the same reason.
  TableLayout nan = layout;
  nan.props.maxHeight = std::nanf("");
  TableLayout nanAgain = nan;
  CHECK(nan == nanAgain, "two NaN props are equal");
  CHECK(!(nan == layout), "a NaN prop is not the default");
}

/// The table box is `toolbar + min(rows * rowHeight, max(cap, floor))`.
///
/// The toolbar is outside the cap, because a capped table is still the one a
/// reader uses to lift the cap; the floor is inside it, because a cap that would
/// show one header row is not a cap anyone can use.
void testTableHeight() {
  const float toolbar = 36;
  // Said explicitly rather than by default-construction, so this test pins the
  // numbers it is about instead of the header's initialisers.
  TableBlockLayout uncapped{0};
  TableBlockLayout capped{300};

  CHECK(tableHeight(uncapped, 2, toolbar) == toolbar + 2 * kTableRowHeight,
        "a table under the cap is toolbar plus rows");
  CHECK(tableHeight(capped, 100, toolbar) == toolbar + 300, "a table over the cap is capped");
  CHECK(tableHeight(capped, 2, toolbar) == toolbar + 2 * kTableRowHeight,
        "a table shorter than the cap is its own height, not the cap");

  // The floor wins over the cap: a cap of one row still shows three, which is a
  // header and two rows a reader can act on.
  TableBlockLayout tiny{static_cast<float>(kTableRowHeight)};
  CHECK(tableHeight(tiny, 100, toolbar) == toolbar + kMinVisibleTableRows * kTableRowHeight,
        "a cap below the floor shows the floor");

  // The floor never adds space to a small table: a two-row table is two rows.
  CHECK(tableHeight(capped, kMinVisibleTableRows - 1, toolbar) == toolbar + (kMinVisibleTableRows - 1) * kTableRowHeight,
        "the floor does not add space to a table shorter than itself");

  // The toolbar is outside the cap in both directions, so a capped table is
  // never shorter than an uncapped one with the same rows.
  CHECK(tableHeight(capped, 100, toolbar) > tableHeight(capped, 100, 0),
        "the toolbar is outside the cap");

  // An expanded table's cap is already 0 by the time it gets here, and it lays
  // out as uncapped rather than as a table at height 0.
  TableBlockLayout expanded;
  expanded.expanded = true;
  CHECK(expanded.maxHeight == 0.0f, "a default-constructed block has no cap");
  CHECK(tableHeight(expanded, 4, toolbar) == tableHeight(uncapped, 4, toolbar),
        "an expanded table is laid out as uncapped");

  // Every row count, capped and uncapped, is never below its own rows and never
  // below the floor, which is the two invariants the platforms rely on.
  const auto gridOf = [](uint32_t rows) { return static_cast<float>(rows) * kTableRowHeight; };
  for (uint32_t rows = 0; rows <= 200; rows++) {
    const float grid = gridOf(rows);
    // Compared against the whole value rather than a difference: `a + b - a` is
    // not `b` in floating point, and the platforms lay the toolbar out separately
    // from the grid anyway, so the whole height is the thing that has to agree.
    const float cappedHeight = tableHeight(capped, rows, toolbar);
    CHECK(cappedHeight - toolbar >= std::min(grid, 300.0f) - 0.001f,
          "a capped table is at least the smaller of its rows and its cap");
    CHECK(cappedHeight - toolbar >= std::min(grid, kMinVisibleTableRows * kTableRowHeight) - 0.001f,
          "a capped table is at least the smaller of its rows and the floor");
    CHECK(cappedHeight <= toolbar + std::min(std::max(grid, kMinVisibleTableRows * kTableRowHeight), 300.0f) + 0.001f,
          "a capped table is no taller than the cap or the floor needs");
    CHECK(tableHeight(uncapped, rows, toolbar) == toolbar + grid, "an uncapped table is its own rows");
  }
}

/// A collapsible block's open state, and the height a closed body takes.
///
/// The reader's answer is the later of the two, so a reader who closes a block
/// the writer opened keeps it closed: the summary is a control, not a caption.
void testDetailsLayout() {
  ReaderChoices none;
  CHECK(!resolveDetails(none, 1, false).open, "a block with neither answer is closed");
  CHECK(resolveDetails(none, 1, true).open, "the writer's open is the starting state");

  ReaderChoices opened = none.with(ChoiceKind::DetailsOpen, 1, true);
  CHECK(resolveDetails(opened, 1, false).open, "a reader's choice opens a block the writer closed");
  CHECK(!resolveDetails(opened, 1, true).open, "and closes one they did not");
  CHECK(!resolveDetails(opened, 2, false).open, "one opened block does not open its neighbour");

  // Closing a block the writer opened is the case that decides the order the two
  // answers are read in: the reader's is the later one, so it wins.
  ReaderChoices closedByReader = none.with(ChoiceKind::DetailsOpen, 3, true);
  CHECK(resolveDetails(closedByReader, 3, false).open, "a reader can open a block the writer left closed");
  ReaderChoices noRecord = none.with(ChoiceKind::DetailsOpen, 4, false);
  CHECK(resolveDetails(noRecord, 4, true).open, "the writer's open stands when the reader said nothing");
  CHECK(resolveDetails(none, 5, true).open, "with no choice at all, the writer decides");
  // A reader can also close a block the writer opened, which is what makes a
  // recorded choice a flip rather than only an open.
  ReaderChoices readerClosed = none.with(ChoiceKind::DetailsOpen, 6, true);
  CHECK(!resolveDetails(readerClosed, 6, true).open, "a reader can close a block the writer opened");

  // The choice sets are separate, so a code toggle cannot open a collapsible
  // block and a collapsible block's toggle cannot unwrap a code block. They share
  // a container id space, which is exactly why they may not share a set.
  ReaderChoices codeOnly = none.with(ChoiceKind::CodeExpand, 1, true);
  CHECK(!resolveDetails(codeOnly, 1, false).open, "a code expand does not open a details block");

  // A closed body is zero tall, whatever the body measured to.
  CHECK(detailsBodyHeight(true, 120) == 120, "an open body is its own height");
  CHECK(detailsBodyHeight(false, 120) == 0, "a closed body takes no space at all");
  CHECK(detailsBodyHeight(false, 0) == 0, "and a body that was empty stays empty");
}

/// The height a text segment shows once its closed bodies are taken out.
///
/// The rule is one sentence — a line goes when its first character is inside a
/// collapsed range — and the rest of this is what that sentence has to be true of.
void testVisibleTextHeight() {
  // Four lines of ten points each, laid out from a paragraph with a blank line
  // between every block: line starts are at the block boundaries.
  const std::vector<float> tops = {0, 10, 30, 50, 60};
  const std::vector<uint32_t> starts = {0, 12, 30, 46};

  CHECK(visibleTextHeight(60, tops, starts, {}) == 60, "nothing hidden is the whole height");
  // The middle block is lines 1 and 2.
  CHECK(visibleTextHeight(60, tops, starts, {{12, 30}}) == 40, "a collapsed block takes its own lines");

  // The first block, which is a summary but could be some other block's body.
  CHECK(visibleTextHeight(60, tops, starts, {{0, 12}}) == 50, "a leading block goes too");
  // The last block: line 3 only, since its range ends at the text's end.
  CHECK(visibleTextHeight(60, tops, starts, {{46, 1000}}) == 50, "a trailing block goes too");

  // Two collapsed blocks add up, and a block that starts on the same line as
  // another is counted once.
  CHECK(visibleTextHeight(60, tops, starts, {{12, 30}, {46, 1000}}) == 30, "two collapsed blocks both go");
  CHECK(visibleTextHeight(60, tops, starts, {{12, 30}, {13, 30}}) == 40, "overlapping ranges count once");

  // A range that ends exactly where a line starts does not take that line, which
  // is what makes a block's last line its own rather than the next block's first.
  CHECK(visibleTextHeight(60, tops, starts, {{12, 30}}) == 40, "a range ending on a line start leaves it");

  // A range that lies wholly inside one line hides nothing: the line belongs to
  // whatever ran into it, and taking it would remove the last line of a visible
  // block along with a body that was never on it. This is what a one-line
  // paragraph between two visible ones does.
  CHECK(visibleTextHeight(60, tops, starts, {{13, 14}}) == 60,
        "a range inside one line leaves that line");
  // One character earlier does take the line, because the line now begins inside
  // the block — which is the ordinary case of a block that starts a line.
  CHECK(visibleTextHeight(60, tops, starts, {{12, 14}}) == 40, "and takes it once the line starts inside");

  // A segment with one line hides it when its only block is collapsed, and a
  // caller with no line table gets the whole height rather than a crash: this is
  // the measurement path, where a crash would be worse than a wrong number.
  CHECK(visibleTextHeight(20, {0, 20}, {0}, {{0, 5}}) == 0, "a one-line segment hides its only line");
  CHECK(visibleTextHeight(20, {}, {}, {{0, 5}}) == 20, "no line table is the whole height");
  CHECK(visibleTextHeight(20, {0}, {0}, {}) == 20, "a single line top is the whole height");
}

/// The layout direction of a block, decided once in the engine.
///
/// The table itself is generated, so what is pinned here is the rule and the
/// table's edges: which characters count, which way a tie goes, and the three
/// cases that were wrong on one platform or the other.
void testDirection() {
  const auto utf8 = [](const char *bytes) { return utf8ToUtf16(bytes); };

  // The table's own edges: an ASCII letter and a Hebrew letter are on the sides
  // they are on, and neither table claims the other's ground.
  CHECK(isStrongLeftToRight('a'), "an ascii letter reads left to right");
  CHECK(!isStrongRightToLeft('a'), "and is not right to left");
  CHECK(isStrongRightToLeft(0x05d0), "a hebrew letter reads right to left");
  CHECK(!isStrongLeftToRight(0x05d0), "and is not left to right");
  CHECK(isStrongRightToLeft(0x0627), "an arabic letter reads right to left");
  // A neutral is in neither table, which is what lets a number-only line
  // inherit rather than being counted.
  CHECK(!isStrongLeftToRight('5'), "a digit is neutral");
  CHECK(!isStrongRightToLeft('5'), "a digit is neutral both ways");
  CHECK(isStrongLeftToRight(0x200e), "the left-to-right mark is strong left to right");
  CHECK(isStrongRightToLeft(0x200f), "and the right-to-left mark the other way");
  // A supplementary letter, which is a surrogate pair in the text and one code
  // point in the table.
  CHECK(isStrongLeftToRight(0x10400), "a deseret letter reads left to right");

  // The rule: the majority wins.
  CHECK(textDirection(utf8("hello world"), BlockDirection::Inherit) == BlockDirection::LeftToRight,
        "an english paragraph is left to right");
  CHECK(textDirection(utf8("\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D \xD7\xA2\xD7\x95\xD7\x9C\xD7\x9D"), BlockDirection::Inherit) ==
            BlockDirection::RightToLeft,
        "a hebrew paragraph is right to left");
  CHECK(textDirection(utf8("one two three \xD7\x90\xD7\x91"), BlockDirection::Inherit) == BlockDirection::LeftToRight,
        "one hebrew letter in english prose does not turn the line");
  CHECK(textDirection(utf8("\xD7\x90\xD7\x91\xD7\x92\xD7\x93\xD7\x94 one"), BlockDirection::Inherit) ==
            BlockDirection::RightToLeft,
        "five hebrew letters against one english word read right to left");

  // A tie goes to the first strong character, because a reader's eye starts at the
  // beginning of the line.
  CHECK(textDirection(utf8("a \xD7\x90"), BlockDirection::Inherit) == BlockDirection::LeftToRight,
        "a tie opens the way its first strong character reads");
  CHECK(textDirection(utf8("\xD7\x90 a"), BlockDirection::Inherit) == BlockDirection::RightToLeft,
        "and the same the other way round");

  // A block with no strong character is handed back the direction it was given,
  // which is what "inherits" means here: the function has no document to look at,
  // so it does not guess. The builder is what carries the previous block's
  // direction forward, and a document's first such block inherits left to right.
  CHECK(textDirection(utf8("123 456"), BlockDirection::Inherit) == BlockDirection::Inherit,
        "a number-only line inherits rather than deciding");
  CHECK(textDirection(utf8("123 456"), BlockDirection::RightToLeft) == BlockDirection::RightToLeft,
        "and inherits right to left where that is what came before");
  CHECK(textDirection(utf8("123 456"), BlockDirection::LeftToRight) == BlockDirection::LeftToRight,
        "and left to right where that is");
  CHECK(textDirection(u"", BlockDirection::RightToLeft) == BlockDirection::RightToLeft,
        "empty text inherits");
  CHECK(textDirection(utf8("... --- ..."), BlockDirection::RightToLeft) == BlockDirection::RightToLeft,
        "punctuation is neutral too");

  // Code and math are skipped rather than counted, which is the case the two
  // platforms disagreed on: a paragraph that opens with an inline code span and
  // then Arabic came out two different ways.
  Block code;
  code.kind = BlockKind::CodeBlock;
  code.text = utf8("\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D");
  CHECK(blockDirection(code, BlockDirection::LeftToRight) == BlockDirection::Inherit,
        "a code block is laid out left to right whatever it contains");
  Block math;
  math.kind = BlockKind::MathBlock;
  math.text = utf8("\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D");
  CHECK(blockDirection(math, BlockDirection::Inherit) == BlockDirection::Inherit,
        "and so is a formula");

  Block prose;
  prose.kind = BlockKind::Paragraph;
  prose.text = utf8("`x` \xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D");
  CHECK(blockDirection(prose, BlockDirection::Inherit) == BlockDirection::RightToLeft,
        "prose that opens with inline code still reads the way its words do");
}

/// A custom container's blocks group into their own segment, and the slot query
/// names them for a renderer.
///
/// The plan groups only simple blocks: code, tables, formulas and pictures keep
/// their own segments wherever they sit, so a container holding any of those is
/// drawn natively rather than hosted.
void checkPlanWith(const char *label, std::string_view source, const ParseOptions &options, const char *expected) {
  const ParseResult result = parseMarkdown(source, options);
  if (result.status != ParseStatus::Ok || !result.document) {
    CHECK(false, label);
    return;
  }
  std::string shape;
  for (const SegmentPlan &segment : planSegments(*result.document)) {
    if (!shape.empty()) {
      shape.push_back(' ');
    }
    switch (segment.kind) {
      case SegmentKind::Table: shape += 'T'; break;
      case SegmentKind::Code: shape += 'C'; break;
      case SegmentKind::Math: shape += 'M'; break;
      case SegmentKind::Image: shape += 'I'; break;
      case SegmentKind::Text: shape += 't'; break;
      case SegmentKind::Custom: shape += 'U'; break;
    }
    shape += std::to_string(segment.from);
    shape.push_back('-');
    shape += std::to_string(segment.to);
  }
  expectEq(shape, expected, label);
}

void testCustomSegments() {
  TagPolicy policy;
  policy.tags = {AllowedTag{"user", {}}};
  // Inline HTML on, as the view resolves it: container pairing is an HTML
  // feature, and without the bit every tag stays source.
  const ParseOptions options(ParseFlags::Default | ParseFlags::InlineHtml, policy);
  // One paragraph in one container: one custom segment.
  checkPlanWith("a custom container groups its blocks", "<user>\n\nHello.\n\n</user>\n", options, "U0-1");
  // Without the policy the tag is not a container, so the opener, the body and
  // the closer stay one text segment of ordinary blocks.
  checkPlan("with no policy there is no custom segment", "<user>\n\nHello.\n\n</user>\n", "t0-3");
  // A fence inside splits: the container spans three segments and is drawn
  // natively rather than hosted, because one slot cannot replace two drawings.
  checkPlanWith("a fence inside splits the container's segments", "<user>\n\nA.\n\n```\nx\n```\n\nB.\n\n</user>\n",
                options, "U0-1 C1-2 U2-3");
}

/// The tag slots in a document: one per custom container whose tag the app
/// named, with the source it holds and whether its closer arrived.
void testTagSlots() {
  TagPolicy policy;
  policy.tags = {AllowedTag{"user", {}}};
  const ParseOptions options(ParseFlags::Default | ParseFlags::InlineHtml, policy);
  const std::string source = "<user>\n\nHello **there**.\n\n</user>\n";
  const ParseResult result = parseMarkdown(source, options);
  CHECK(result.status == ParseStatus::Ok, "the custom document parses");
  // A controlled parse treats every block as settled, the way the fence query
  // does: the source is complete, so nothing in it is still arriving.
  const uint32_t settled = static_cast<uint32_t>(result.document->blocks.size());
  const std::vector<TagSlot> slots = tagSlots(*result.document, source, {"user"}, false, settled);
  CHECK(slots.size() == 1, "one container is one slot");
  CHECK(slots[0].name == "user", "the slot names its tag");
  CHECK(slots[0].markdown == "Hello **there**.", "the slot holds the body's source, not a re-serialization");
  CHECK(!slots[0].incomplete, "a closed container is complete");
  const auto liveClosed = tagSlots(*result.document, source, {"user"}, true);
  CHECK(liveClosed.size() == 1 && !liveClosed[0].incomplete,
        "the last closed container is complete even while the stream is live");
  CHECK(slots[0].blockCount == 1, "and it spans its one block");
  // A tag with no renderer draws natively, so there is nothing to mount for it.
  CHECK(tagSlots(*result.document, source, {"team"}, false, settled).empty(), "an unnamed tag is no slot");
  CHECK(tagSlots(*result.document, source, {}, false, settled).empty(), "no names is no slots");
  // No closer yet: the body is still arriving, so the renderer shows a
  // placeholder rather than half a card.
  const ParseResult open = parseMarkdown("<user>\n\nHello.\n", options);
  CHECK(open.status == ParseStatus::Ok, "the unclosed document parses");
  const std::vector<TagSlot> pending = tagSlots(*open.document, "<user>\n\nHello.\n", {"user"}, true);
  CHECK(pending.size() == 1, "an open container is still a slot");
  CHECK(pending[0].incomplete, "but an incomplete one");
  const auto controlledOpen = tagSlots(*open.document, "<user>\n\nHello.\n", {"user"}, true,
                                       static_cast<uint32_t>(open.document->blocks.size()));
  CHECK(controlledOpen.size() == 1 && controlledOpen[0].incomplete,
        "a controlled unclosed container stays incomplete even with a settled prefix");
  const auto finishedOpen = tagSlots(*open.document, "<user>\n\nHello.\n", {"user"}, false,
                                    static_cast<uint32_t>(open.document->blocks.size()));
  CHECK(finishedOpen.size() == 1 && !finishedOpen[0].incomplete, "finish settles a container without a closer");
}

/// The gutter is as wide as the widest number it draws, and never narrower than
/// two digits, which is what keeps Streamdown\'s "over 100" bug out.
void testGutterDigits() {
  CHECK(gutterDigits(1, 1) == 2, "one line is at least two digits");
  CHECK(gutterDigits(1, 9) == 2, "nine lines are two digits");
  CHECK(gutterDigits(1, 10) == 2, "line ten is still two digits");
  CHECK(gutterDigits(1, 99) == 2, "line 99 is two digits");
  CHECK(gutterDigits(1, 100) == 3, "line 100 is three digits");
  CHECK(gutterDigits(1, 1000) == 4, "line 1000 is four digits");
  // The last number decides, not the first, so a block that starts at 998 shows
  // 998, 999, and 1000 in a gutter wide enough for all of them.
  CHECK(gutterDigits(998, 3) == 4, "a start line past 999 widens the gutter");
  CHECK(gutterDigits(998, 1) == 3, "a start line of 998 on its own is three digits");
  CHECK(gutterDigits(1000000, 1) == 7, "the largest start line is seven digits");
  // An empty fence draws one numbered line, like Streamdown's empty row.
  CHECK(gutterDigits(7, 0) == 2, "an empty fence numbers its one line");
  CHECK(gutterDigits(7, 0) == gutterDigits(7, 1), "an empty fence is one line");
}

/// The card height is `header + min(body + 2 * padding, cap)`, with a floor at
/// five lines so a cap cannot shrink a block to two lines at an accessibility
/// text size.
void testCodeCardHeight() {
  const float header = 32;
  const float padding = 10;
  const float line = 16;
  CodeBlockLayout block;
  block.maxHeight = 400;

  CHECK(codeCardHeight(block, header, padding, 100, line) == header + 100 + 2 * padding,
        "a body under the cap is header plus body plus padding");
  CHECK(codeCardHeight(block, header, padding, 1000, line) == header + 400, "a body over the cap is capped");
  CHECK(codeCardHeight(block, 0, padding, 1000, line) == 400, "no header means no header");

  CodeBlockLayout uncapped = block;
  uncapped.maxHeight = 0;
  CHECK(codeCardHeight(uncapped, header, padding, 1000, line) == header + 1000 + 2 * padding,
        "a zero cap grows with the body");

  // The floor: a cap below five lines plus padding is raised to it, so at the
  // largest text size a capped block is still five lines rather than two.
  const float largeLine = 40;
  CodeBlockLayout small = block;
  small.maxHeight = 200;
  CHECK(codeCardHeight(small, header, padding, 1000, largeLine) == header + 5 * largeLine + 2 * padding,
        "the floor is five lines plus padding");
  // A cap above the floor is not lowered by it.
  CHECK(codeCardHeight(block, header, padding, 100, line) == header + 100 + 2 * padding,
        "the floor never grows a card past its body");

  // A body shorter than the floor lays out at its own height: the floor raises a
  // cap, and it is not a minimum height for the card itself.
  CHECK(codeCardHeight(small, header, padding, 40, line) == header + 40 + 2 * padding,
        "the floor does not pad a short block");
}

/// Logical line starts, which the gutter walks. An empty fence still has one.
void testCodeLineStarts() {
  const std::vector<uint32_t> single = codeLineStarts(u"a");
  CHECK(single.size() == 1 && single[0] == 0, "one line starts at zero");

  const std::vector<uint32_t> three = codeLineStarts(u"a\nb\nc");
  CHECK(three.size() == 3 && three[0] == 0 && three[1] == 2 && three[2] == 4, "a start after each newline");

  // A trailing newline ends a line the writer wrote, so it starts one.
  const std::vector<uint32_t> trailing = codeLineStarts(u"a\n");
  CHECK(trailing.size() == 2 && trailing[1] == 2, "a trailing newline starts a last empty line");

  const std::vector<uint32_t> empty = codeLineStarts(u"");
  CHECK(empty.size() == 1 && empty[0] == 0, "an empty block has one line to number");
}

// ------------------------------------------------------------- code export

void testCodeExport() {
  checkExport("const a = 1;\nconst b = 2;", "code text is verbatim", "```ts title=\"app.ts\"\nconst a = 1;\nconst b = 2;\n```\n",
              ExportFormat::Text);
  checkExport("", "an empty fence copies nothing", "```\n```\n", ExportFormat::Text);
  checkExport("a\tb", "a tab is kept", "```\na\tb\n```\n", ExportFormat::Text);
  checkExport("a\nb", "CRLF leaves one newline", "```\r\na\r\nb\r\n```\r\n", ExportFormat::Text);
  // A code block arrives as a counted verbatim line, so its NUL is a byte in
  // the middle of the text rather than a report of its own. It still displays,
  // and still copies, as the replacement character.
  checkExport("a\357\277\275b", "NUL becomes a replacement character", std::string("```\na\0b\n```\n", 12),
              ExportFormat::Text);
  checkExport("a\360\237\241\213\360\237\232\200", "an astral emoji survives", "```\na\360\237\241\213\360\237\232\200\n```\n",
              ExportFormat::Text);
  // Container prefixes are already gone from the model, so a copy is the code
  // and not the quoted or indented source it was written as.
  checkExport("x", "code in a list item loses the marker", "- ```\n  x\n  ```\n", ExportFormat::Text);
  checkExport("x", "code in a quote loses the marker", "> ```\n> x\n> ```\n", ExportFormat::Text);
  checkExport("x", "code in a nested quote in a list loses every marker", "> - ```\n>   x\n>   ```\n", ExportFormat::Text);
  checkExport("// see https://example.com", "a line that looks like a link stays text",
              "```\n// see https://example.com\n```\n", ExportFormat::Text);
  checkExport("+[->+<]", "an unknown language is still text", "```brainfuck\n+[->+<]\n```\n", ExportFormat::Text);
  checkExport("", "an empty range is empty", "", ExportFormat::Text);
}

// ------------------------------------------------------------- CSV and TSV

void testDelimited() {
  const char *table = "| Name | Age | City |\n| --- | --- | --- |\n| John | 30 | New York |\n";
  checkExport("Name,Age,City\nJohn,30,New York", "a simple table is comma separated", table, ExportFormat::Csv);
  checkExport("Name\tAge\tCity\nJohn\t30\tNew York", "a simple table is tab separated", table, ExportFormat::Tsv);
  checkExport("a,b\nx,\"y, z\"", "a comma in a value is quoted", "| a | b |\n| - | - |\n| x | y, z |\n", ExportFormat::Csv);
  checkExport("m\n\"Hello, \"\"World\"\"\"", "quotes and the separator are quoted together",
              "| m |\n| - |\n| Hello, \"World\" |\n", ExportFormat::Csv);
  checkExport("q\n\"He said \"\"Hello\"\"\"", "a quote is doubled", "| q |\n| - |\n| He said \"Hello\" |\n",
              ExportFormat::Csv);
  checkExport("a;b\nx;\"y; z\"", "a semicolon separator quotes semicolons", "| a | b |\n| - | - |\n| x | y; z |\n",
              ExportFormat::Csv, u';');
  checkExport("a\tb\nx\t\"y\tz\"", "a tab separator quotes tabs", "| a | b |\n| - | - |\n| x | y\tz |\n", ExportFormat::Csv,
              u'\t');
  checkExport("n\nJohn", "one column never shows the separator", "| n |\n| - |\n| John |\n", ExportFormat::Csv, u';');
  checkExport("t\nA\\tB", "a tab in TSV is an escape", "| t |\n| - |\n| A\tB |\n", ExportFormat::Tsv);
  // md4c gives every row the header's column count, so a source row with fewer
  // cells arrives as one with an empty trailing field. It is written as it is,
  // rather than trimmed, so every row keeps the same number of fields.
  checkExport("a,b\n1,", "a short source row is a row with an empty cell", "| a | b |\n| - | - |\n| 1 |\n",
              ExportFormat::Csv);
  checkExport("a,b", "a header-only table is one row", "| a | b |\n| - | - |\n", ExportFormat::Csv);
  checkExport("a,b\n,", "empty values are empty fields", "| a | b |\n| - | - |\n|  |  |\n", ExportFormat::Csv);
  checkExport("", "an empty table is empty", "", ExportFormat::Csv);
  checkExport("t\nA, B", "TSV does not quote", "| t |\n| - |\n| A, B |\n", ExportFormat::Tsv);
}

// ----------------------------------------------------------------- Markdown

void testMarkdown() {
  checkExport("| a | b |\n| --- | --- |\n| 1 | 2 |", "a simple table round trips", "| a | b |\n| - | - |\n| 1 | 2 |\n",
              ExportFormat::Markdown);
  checkExport("| a | b | c |\n| :-- | :-: | --: |\n| 1 | 2 | 3 |", "alignment is carried",
              "| a | b | c |\n| :-- | :-: | --: |\n| 1 | 2 | 3 |\n", ExportFormat::Markdown);
  checkExport("| a |\n| --- |\n| x \\| y |", "a pipe is escaped", "| a |\n| - |\n| x \\| y |\n", ExportFormat::Markdown);
  checkExport("| a | b |\n| --- | --- |\n| 1 |  |", "a short row is padded", "| a | b |\n| - | - |\n| 1 |\n",
              ExportFormat::Markdown);
  checkExport("| a |\n| --- |\n| **b** |", "strong is written back", "| a |\n| - |\n| **b** |\n", ExportFormat::Markdown);
  checkExport("| a |\n| --- |\n| *b* |", "emphasis is written back", "| a |\n| - |\n| *b* |\n", ExportFormat::Markdown);
  checkExport("| a |\n| --- |\n| ~~b~~ |", "strikethrough is written back", "| a |\n| - |\n| ~~b~~ |\n",
              ExportFormat::Markdown);
  checkExport("| a |\n| --- |\n| `b` |", "code is written back", "| a |\n| - |\n| `b` |\n", ExportFormat::Markdown);
  checkExport("| a |\n| --- |\n| **`x`** |", "strong around code nests the other way round",
              "| a |\n| - |\n| **`x`** |\n", ExportFormat::Markdown);
  checkExport("| a |\n| --- |\n| [x](https://e) |", "a link is written back", "| a |\n| - |\n| [x](https://e) |\n",
              ExportFormat::Markdown);
  checkExport("| a |\n| --- |\n| [*x*](https://e) |", "emphasis inside a link nests inside it",
              "| a |\n| - |\n| [*x*](https://e) |\n", ExportFormat::Markdown);
  checkExport("| a |\n| --- |\n| x |", "a rejected target is plain text", "| a |\n| - |\n| [x](javascript:y) |\n",
              ExportFormat::Markdown);
  // Visible text has its Markdown metacharacters escaped, so it re-reads as the
  // words the reader saw rather than as new syntax.
  checkExport("| a |\n| --- |\n| x\\_y |", "metacharacters are escaped", "| a |\n| - |\n| x_y |\n", ExportFormat::Markdown);
  checkExport("| a |\n| --- |\n| x \\< y \\& z |", "angle brackets and ampersands are escaped",
              "| a |\n| - |\n| x < y & z |\n", ExportFormat::Markdown);
  checkExport("| a |\n| --- |\n| \\[x\\] |", "brackets are escaped", "| a |\n| - |\n| [x] |\n", ExportFormat::Markdown);
  // An image is its alt text today, so that is all there is to write back.
  checkExport("| a |\n| --- |\n| alt |", "image alt text is plain text", "| a |\n| - |\n| ![alt](https://e/x.png) |\n",
              ExportFormat::Markdown);
  checkExport("| a | b |\n| --- | --- |\n| x1 | 2 |", "a footnote reference is the number the reader sees",
              "| a | b |\n| - | - |\n| x[^1] | 2 |\n\n[^1]: y\n", ExportFormat::Markdown, u',', 2);
  // A GFM header is bold because it is a header, not because the writer wrote
  // `**`, and the model cannot tell the two apart, so the export does not put it
  // back. The destination bolds a header row anyway, so nothing is lost.
  checkExport("| a | b |\n| :-- | --: |\n| 1 | 2 |", "a header row is not written back as bold",
              "| a | b |\n| :-- | --: |\n| 1 | 2 |\n", ExportFormat::Markdown);
  checkExport("| a | *b* |\n| --- | --- |\n| 1 | 2 |", "a style the writer did put in a header survives",
              "| **a** | *b* |\n| - | - |\n| 1 | 2 |\n", ExportFormat::Markdown);
  checkExport("", "an empty table exports nothing", "", ExportFormat::Markdown);

  // A code span's fence is longer than anything inside it, and the one space of
  // padding goes inside the fence. That is where CommonMark looks for it, and it
  // is what keeps the closing run from merging with the content's own trailing
  // backtick and becoming unclosable. The bytes cannot be one readable literal:
  // the same characters read as something else entirely, so each case is
  // spelled out.
  {
    const std::string tick(1, '`');
    const std::string source = "| a |\n| - |\n| " + std::string(2, '`') + " " + tick + "x" + tick + " " +
                               std::string(2, '`') + " |\n";
    checkExport("| a |\n| --- |\n| " + std::string(2, '`') + " " + tick + "x" + tick + " " + std::string(2, '`') + " |",
                "a code span holding a backtick is fenced wider and padded", source, ExportFormat::Markdown);
  }
  {
    const std::string tick(1, '`');
    const std::string pair = std::string(2, '`');
    const std::string source = "| a |\n| - |\n| " + std::string(3, '`') + " " + pair + "x" + pair + " " +
                               std::string(3, '`') + " |\n";
    checkExport("| a |\n| --- |\n| " + std::string(3, '`') + " " + pair + "x" + pair + " " + std::string(3, '`') + " |",
                "a code span holding a double backtick needs a triple fence", source, ExportFormat::Markdown);
  }
}

// -------------------------------------------------------------------- HTML

void testHtml() {
  checkExport("<table><thead><tr><th>a</th><th>b</th></tr></thead><tbody><tr><td>1</td><td>2</td></tr></tbody></table>",
              "a table is a table element", "| a | b |\n| - | - |\n| 1 | 2 |\n", ExportFormat::Html);
  // md4c carries a column's alignment onto every cell, so a body cell is styled
  // the same way its header is and a paste keeps the columns where they were.
  checkExport("<table><thead><tr><th style=\"text-align:left\">a</th><th style=\"text-align:right\">b</th></tr>"
              "</thead><tbody><tr><td style=\"text-align:left\">1</td><td style=\"text-align:right\">2</td></tr>"
              "</tbody></table>",
              "alignment becomes an inline style", "| a | b |\n| :-- | --: |\n| 1 | 2 |\n", ExportFormat::Html);
  checkExport("<table><thead><tr><th>a</th></tr></thead><tbody><tr><td><strong>b</strong> <em>c</em> <del>d</del> "
              "<code>e</code></td></tr></tbody></table>",
              "inline styles become elements", "| a |\n| - |\n| **b** *c* ~~d~~ `e` |\n", ExportFormat::Html);
  checkExport("<table><thead><tr><th>a</th></tr></thead><tbody><tr><td><a href=\"https://e\">x</a></td></tr></tbody></table>",
              "an accepted link becomes an anchor", "| a |\n| - |\n| [x](https://e) |\n", ExportFormat::Html);
  checkExport("<table><thead><tr><th>a</th></tr></thead><tbody><tr><td>x</td></tr></tbody></table>",
              "a rejected target is plain text", "| a |\n| - |\n| [x](javascript:y) |\n", ExportFormat::Html);
  // A cell holding markup is text the reader saw, so it is escaped rather than
  // pasted back as markup.
  checkExport("<table><thead><tr><th>a</th></tr></thead><tbody><tr><td>&lt;script&gt;x()&lt;/script&gt;</td></tr></tbody>"
              "</table>",
              "text is escaped", "| a |\n| - |\n| <script>x()</script> |\n", ExportFormat::Html);
  checkExport("<table><thead><tr><th>a</th></tr></thead><tbody><tr><td>he said &quot;hi&quot;</td></tr></tbody></table>",
              "quotes in text are escaped", "| a |\n| - |\n| he said \"hi\" |\n", ExportFormat::Html);
  checkExport("", "an empty table exports nothing", "", ExportFormat::Html);
}

/// A script run and a cell line break, exported in every format that has to
/// carry them, each one round-tripped through the parser so the claim is that a
/// paste reproduces the document rather than that a string matches.
void testScriptExports() {
  const ParseOptions inlineHtml{ParseFlags::InlineHtml};
  const auto exported = [&](std::string_view source, ExportFormat format) {
    const ParseResult result = parseMarkdown(source, inlineHtml);
    if (result.status != ParseStatus::Ok) {
      return std::string("<no document>");
    }
    return narrow(exportBlocks(*result.document, 0, result.document->blocks.size(), format));
  };

  // Markdown writes the tags back unescaped, which is the one place a `<` in a
  // table export is not escaped: a reader that does not know them sees the same
  // literal text it started from.
  check("| a |\n| --- |\n| E=mc<sup>2</sup> |", "a superscript is written back as a tag",
        exported("| a |\n| - |\n| E=mc<sup>2</sup> |\n", ExportFormat::Markdown));
  check("| a |\n| --- |\n| H<sub>2</sub>O |", "a subscript is written back as a tag",
        exported("| a |\n| - |\n| H<sub>2</sub>O |\n", ExportFormat::Markdown));
  check("| a |\n| --- |\n| **<sup>x</sup>** |", "a script wraps the emphasis it was written around",
        exported("| a |\n| - |\n| <sup>**x**</sup> |\n", ExportFormat::Markdown));
  check("| a |\n| --- |\n| <sup>`x`</sup> |", "a raised code span keeps the tag outside its backticks",
        exported("| a |\n| - |\n| <sup>`x`</sup> |\n", ExportFormat::Markdown));
  check("| a |\n| --- |\n| Mix<br>Then |", "a cell break is written back as a br",
        exported("| a |\n| - |\n| Mix<br>Then |\n", ExportFormat::Markdown));

  // HTML writes the same runs as elements, and a cell break as a tag rather
  // than a raw newline, which a browser would collapse to a space.
  check("<table><thead><tr><th>a</th></tr></thead><tbody><tr><td>E=mc<sup>2</sup></td></tr></tbody></table>",
        "HTML writes a superscript as an element",
        exported("| a |\n| - |\n| E=mc<sup>2</sup> |\n", ExportFormat::Html));
  check("<table><thead><tr><th>a</th></tr></thead><tbody><tr><td>Mix<br>Then</td></tr></tbody></table>",
        "HTML writes a cell break as a br",
        exported("| a |\n| - |\n| Mix<br>Then |\n", ExportFormat::Html));

  // CSV and TSV carry the text, which is Streamdown's own behavior for a
  // superscript and for a break in a cell.
  check("a\nx2", "CSV writes a superscript as its text",
        exported("| a |\n| - |\n| x<sup>2</sup> |\n", ExportFormat::Csv));
  check("a\n\"Mix\nThen\"", "CSV quotes a cell that holds a break",
        exported("| a |\n| - |\n| Mix<br>Then |\n", ExportFormat::Csv));
  check("a\nMix\\nThen", "TSV escapes a cell break",
        exported("| a |\n| - |\n| Mix<br>Then |\n", ExportFormat::Tsv));

  // The round trip: a Markdown export of a document with scripts and a break
  // parses back to the same visible text and the same runs.
  {
    const std::string source = "| Step | Notes |\n| - | - |\n| 1 | Mix **x**<sup>2</sup><br>Then bake |\n";
    const std::string markdown = exported(source, ExportFormat::Markdown);
    const ParseResult again = parseMarkdown(markdown, inlineHtml);
    CHECK(again.status == ParseStatus::Ok, "the Markdown export re-parses");
    // The first block is the header row the export writes back; the body row is
    // the one that held the script and the break.
    if (again.status == ParseStatus::Ok && again.document->blocks.size() >= 2) {
      const Block &row = again.document->blocks[1];
      const TableCell &cell = row.cells.back();
      expectEq(narrow(row.text.substr(cell.start, cell.end - cell.start)), "Mix x2\nThen bake",
               "a round trip keeps the text and the break");
      bool raised = false;
      for (const InlineRun &run : row.runs) {
        if (run.start < cell.end && run.end > cell.start && (run.styles & InlineStyle::Superscript) != 0) {
          raised = true;
        }
      }
      CHECK(raised, "a round trip keeps the superscript");
    }
  }
  {
    const std::string source = "| Step | Notes |\n| - | - |\n| 1 | Mix<br>Then bake |\n";
    const std::string html = exported(source, ExportFormat::Html);
    // A browser reads `<br>` as a break, so the export is the HTML a reader of
    // the paste would see: the break survives where a raw newline would not.
    CHECK(html.find("<br>") != std::string::npos, "the HTML export carries the break as a tag");
  }
}

void testCellPieces() {
  Document document;
  Block row;
  row.kind = BlockKind::TableRow;
  row.text = u"aXb\tcYd\tend";
  row.cells = {{0, 3}, {4, 7}, {8, 11}, {11, 100}, {100, 101}};
  row.runs = {{1, 2, InlineStyle::Strong}, {4, 6, InlineStyle::Emphasis}};
  document.blocks.push_back(row);
  check("| a**X**b | *cY*d | end |  |  |\n| --- | --- | --- | --- | --- |",
        "binary search keeps styled pieces, gaps, and clamped empty cells",
        narrow(exportBlocks(document, 0, 1, ExportFormat::Markdown)));
  check("<table><tbody><tr><td>a<strong>X</strong>b</td><td><em>cY</em>d</td><td>end</td><td></td><td></td></tr>"
        "</tbody></table>", "HTML visits the same styled pieces and empty cells",
        narrow(exportBlocks(document, 0, 1, ExportFormat::Html)));
  Document spanning;
  row.text = u"ab\tcd";
  row.cells = {{0, 2}, {3, 5}};
  row.runs = {{1, 4, InlineStyle::Strong}};
  spanning.blocks.push_back(row);
  check("| a**b** | **c**d |\n| --- | --- |", "a run spanning cells is clipped to each cell",
        narrow(exportBlocks(spanning, 0, 1, ExportFormat::Markdown)));
}

// --------------------------------------------------------------- file names

void testNames() {
  CHECK(imageDownloadName("https://example.com/photo?id=7", "A lake", "image/jpeg", "image") == "A lake.jpeg",
        "an extensionless image keeps the MIME suffix while the caption is built");
  CHECK(imageDownloadName("https://example.com/photo.webp?size=2", "chart.png", "image/jpeg", "image") == "chart.webp",
        "an image URL suffix wins over MIME and replaces the caption suffix");
  CHECK(imageDownloadName("https://example.com/photo", "", "", "saved") == "saved.png",
        "an unnamed image uses the configured base and default suffix");
  checkName("a fence title wins", "app.ts", "ts title=\"app.ts\"", "ts");
  checkName("only the last path part is kept", "c.ts", "ts title='a/b/c.ts'", "ts");
  checkName("a title without an extension gets one", "app.ts", "ts title=app", "ts");
  checkName("a lang:path is a name", "app.ts", "ts:src/app.ts", "ts");
  checkName("reserved characters in a title are removed", "abcdefg.ts", "ts title=\"a:b|c?d*e<f>g\"", "ts");
  checkName("the configured base is next", "snippet.ts", "ts", "ts", "snippet");
  checkName("the fallback is last", "file.ts", "ts", "ts");
  checkName("a configured base is a path too", "snippet.ts", "ts", "ts", "a/b/snippet");
  checkName("a leading dot is dropped", "hidden.ts", "ts", "ts", ".hidden");
  checkName("an unknown language gets txt", "file.txt", "brainfuck", "brainfuck");
  checkName("indented code gets txt", "file.txt", "", "");
  checkName("the language map supplies md", "file.md", "md", "markdown");
  checkName("the language map supplies py", "file.py", "py", "python");
  checkName("the language map supplies rs", "file.rs", "rs", "rust");
  checkName("the language map supplies rb for ruby", "file.rb", "rb", "ruby");
  // A non-ASCII language key is a real fence language, and dropping it would
  // silently save every such file as plain text. The bytes are U+6587 U+8A00,
  // written out because a source literal is not portable.
  const std::string wenyan = "\xe6\x96\x87\xe8\xa8\x80";
  checkName("a non-ASCII language keeps its own extension", "file.wy", wenyan, wenyan);
  checkName("an unknown language with a configured base", "snippet.txt", "", "", "snippet");
  // A fence is written by hand, so a capitalised language is ordinary. The
  // lookup folds ASCII case so it still names the file the writer meant.
  checkName("a capitalised language still names the right file", "file.ts", "TS", "TS");
  checkName("a mixed-case language still names the right file", "file.py", "PyThOn", "PyThOn");
  checkName("a capitalised language keeps a fence title", "app.ts", "TS title=\"app.ts\"", "TS");
  checkName("a language that is only case still resolves", "file.js", "Js", "Js");

  const std::string longName(200, 'a');
  CHECK(downloadName(parseFenceMeta("ts"), "ts", longName, "file").size() == 96 + 3, "a long name is capped");
  const std::string table = tableDownloadName("report", "csv");
  expectEq(table, "report.csv", "a table name takes its extension");
  const std::string unnamed = tableDownloadName("", "md");
  expectEq(unnamed, "table.md", "an unnamed table is table.md");
  const std::string nested = tableDownloadName("a/b/report", "md");
  expectEq(nested, "report.md", "a table base is a path too");
}

} // namespace

int main() {
  const std::vector<InlineRun> runs = {{2, 5, InlineStyle::Strong, -1},
                                       {5, 8, InlineStyle::Emphasis, -1},
                                       {10, 15, InlineStyle::Link, 0}};
  // Compare every boundary and gap against a full scan, including empty and
  // reversed ranges. The renderer and exporter share this selection.
  for (uint32_t start = 0; start <= 16; ++start) {
    for (uint32_t end = 0; end <= 16; ++end) {
      std::vector<const InlineRun *> expected;
      for (const InlineRun &run : runs) {
        if (start < end && run.end > start && run.start < end) expected.push_back(&run);
      }
      const auto selected = inlineRunsInRange(runs, start, end);
      CHECK(selected.size() == expected.size(), "inline range matches full scan count");
      for (size_t index = 0; index < selected.size() && index < expected.size(); ++index) {
        CHECK(&selected[index] == expected[index], "inline range preserves run identity and order");
      }
    }
  }
  CHECK(inlineRunsInRange({}, 0, 10).empty(), "an empty run list has no overlaps");
  testSegmentPlan();
  testMathDocuments();
  testWrapChoices();
  testCodeBlockLayout();
  testTableLayout();
  testTableHeight();
  testDetailsLayout();
  testVisibleTextHeight();
  testDirection();
  testCustomSegments();
  testTagSlots();
  testGutterDigits();
  testCodeCardHeight();
  testCodeLineStarts();
  testCodeExport();
  testDelimited();
  testMarkdown();
  testHtml();
  testScriptExports();
  testCellPieces();
  testNames();
  return ds_check_result();
}
