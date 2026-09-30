// Engine checks for the copy and download controls that no other host binary
// covers: the segment plan a code card depends on, and the export formats and
// file names every platform writes from.
//
//     node --test src/cpp/tests/engine.test.mjs   (runs test_export)

#include "CodeBlocks.h"
#include "CodeHighlighter.h"
#include "Controls.h"
#include "Export.h"
#include "MarkdownParser.h"
#include "SegmentPlan.h"
#include "Unicode.h"
#include "tests/check.h"

#include <algorithm>
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
    shape += segment.isTable() ? 'T' : segment.isCode() ? 'C' : 't';
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
  check(expected, label, downloadName(info, language, std::string(configured), std::string(fallback)));
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
  checkPlan("an empty document has no segments", "", "");
}

// ------------------------------------------------------------- wrap choices

/// A block's layout is one membership test against the toggled set, and both
/// platforms resolve it through this one rule. The cases below are the whole
/// table, so a platform's mirror of it can be checked against them.
void testWrapChoices() {
  const std::vector<uint32_t> none;
  const std::vector<uint32_t> third{2};

  // Nothing toggled: every block follows the prop.
  CHECK(codeWraps(false, none, 0) == false, "an untoggled block does not wrap when the prop is off");
  CHECK(codeWraps(true, none, 0) == true, "an untoggled block wraps when the prop is on");

  // One block toggled: it is the only one that differs from the prop, and the
  // rest still follow it. This is the case a single chrome bit cannot express.
  CHECK(codeWraps(false, third, 2) == true, "a toggled block wraps when the prop is off");
  CHECK(codeWraps(true, third, 2) == false, "a toggled block stops wrapping when the prop is on");
  CHECK(codeWraps(false, third, 0) == false, "an untoggled neighbour still follows the prop being off");
  CHECK(codeWraps(true, third, 0) == true, "an untoggled neighbour still follows the prop being on");
  CHECK(codeWraps(false, third, 1) == false, "a neighbour before the toggle is unaffected");
  CHECK(codeWraps(false, third, 3) == false, "a neighbour after the toggle is unaffected");

  // The chrome bit says only whether every block wraps, and the toggle set is
  // what fills in a mixed document.
  CHECK((chromeForProps(ControlBits::CodeWrap, true, none) & kChromeCodeWrap) != 0,
        "no toggles with the prop on is all-wrapping chrome");
  CHECK((chromeForProps(ControlBits::CodeWrap, true, third) & kChromeCodeWrap) == 0,
        "any toggle clears the all-wrapping bit");
  CHECK((chromeForProps(ControlBits::CodeWrap, false, third) & kChromeCodeWrap) == 0,
        "the prop off is never all-wrapping chrome");

  // The header and the toolbar do not depend on any of this.
  CHECK((chromeForProps(ControlBits::CodeCopy, false, third) & kChromeCodeHeader) != 0,
        "a copy control still gives a code block its header");
  CHECK((chromeForProps(ControlBits::TableDownload, false, none) & kChromeTableToolbar) != 0,
        "a table download still gives a table its toolbar");
  CHECK((chromeForProps(0, false, none) & (kChromeCodeHeader | kChromeTableToolbar)) == 0,
        "no controls means no chrome at all");
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
  CHECK(downloadName("ts", "ts", longName, "file").size() == 96 + 3, "a long name is capped");
  const std::string table = tableDownloadName("report", "csv");
  expectEq(table, "report.csv", "a table name takes its extension");
  const std::string unnamed = tableDownloadName("", "md");
  expectEq(unnamed, "table.md", "an unnamed table is table.md");
  const std::string nested = tableDownloadName("a/b/report", "md");
  expectEq(nested, "report.md", "a table base is a path too");
}

} // namespace

int main() {
  testSegmentPlan();
  testWrapChoices();
  testCodeExport();
  testDelimited();
  testMarkdown();
  testHtml();
  testCellPieces();
  testNames();
  return ds_check_result();
}
