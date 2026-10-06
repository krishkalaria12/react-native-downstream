// The HTML subset, through the real parser.
//
// `Scan.cpp`'s table and `MarkdownParser.cpp`'s application of it are one
// decision split in two, and neither half proves the other: a table can classify
// a tag correctly while the builder ignores it. Every case here goes through
// `parseMarkdown`, so a row that the builder never reached would be caught.

#include "MarkdownParser.h"
#include "Scan.h"
#include "Unicode.h"
#include "check.h"

#include <string>
#include <vector>

using namespace downstream;
using downstream::scan::classifyTag;

namespace {

/// Inline HTML on, nothing else, so a case about a tag is a case about a tag.
const ParseOptions &html() {
  static const ParseOptions options{ParseFlags::InlineHtml};
  return options;
}

ParseResult parse(std::string_view source, ParseOptions options = html()) {
  return parseMarkdown(source, options);
}

bool oneParagraphIs(const std::string &source, const char16_t *expected) {
  const ParseResult result = parse(source);
  if (result.status != ParseStatus::Ok || result.document->blocks.size() != 1) {
    return false;
  }
  return result.document->blocks[0].text == expected;
}

/// The style mask over `substring` in the first block, or 0 when it has no run.
uint32_t stylesOver(const ParseResult &result, const char16_t *substring) {
  const std::u16string &text = result.document->blocks[0].text;
  const size_t at = text.find(substring);
  if (at == std::u16string::npos) {
    return 0;
  }
  uint32_t bits = 0;
  for (const InlineRun &run : result.document->blocks[0].runs) {
    if (run.start < at + std::u16string(substring).size() && run.end > at) {
      bits |= run.styles;
    }
  }
  return bits;
}

/// The table's rows of names and actions, as the tests above read them.
bool tableRows() {
  const TagPolicy plain = defaultTagPolicy();
  const auto row = [&](std::string_view text) { return classifyTag(text, plain); };

  CHECK(row("<br>").style == TagStyle::Break, "br is a break");
  CHECK(row("<strong>").style == TagStyle::Strong, "strong is strong");
  CHECK(row("</strong>").style == TagStyle::Strong && row("</strong>").closing, "and closes");
  CHECK(row("<mark>").style == TagStyle::Highlight, "mark highlights");
  CHECK(row("<u>").style == TagStyle::Underline, "u underlines");
  CHECK(row("<ins>").style == TagStyle::Underline, "ins underlines");
  CHECK(row("<kbd>").style == TagStyle::Keyboard, "kbd is a key cap");
  CHECK(row("<span>").action == TagAction::Unwrap, "span is unwrapped");
  CHECK(row("<script>").action == TagAction::DropContent, "script drops with its content");
  CHECK(row("<!-- x -->").action == TagAction::Hide, "a comment emits nothing");
  CHECK(row("<T>").action == TagAction::Literal, "a generic stays literal");
  CHECK(row("<div>").action == TagAction::Unwrap, "a real element with no style is unwrapped");
  CHECK(row("<a href=\"https://x\">").href == "https://x", "a quoted href is read");
  CHECK(row("<a href=https://x>").href == "https://x", "an unquoted href is read");
  return true;
}

/// The styles the table defines, through the builder.
bool stylesRun() {
  CHECK(oneParagraphIs("a <b>bold</b>", u"a bold"), "b is hidden and its text kept");
  CHECK(stylesOver(parse("a <b>bold</b>"), u"bold") == InlineStyle::Strong, "and sets strong");
  CHECK(oneParagraphIs("<mark>m</mark>", u"m"), "mark is hidden");
  CHECK(stylesOver(parse("<mark>m</mark>"), u"m") == InlineStyle::Highlight, "and sets highlight");
  CHECK(stylesOver(parse("<u>u</u>"), u"u") == InlineStyle::Underline, "u underlines");
  CHECK(stylesOver(parse("<ins>i</ins>"), u"i") == InlineStyle::Underline, "ins underlines");
  CHECK(stylesOver(parse("<kbd>k</kbd>"), u"k") == InlineStyle::Keyboard, "kbd is a key cap");
  // Nesting composes: a marked underline is both.
  CHECK(stylesOver(parse("<mark><u>x</u></mark>"), u"x") ==
            (InlineStyle::Highlight | InlineStyle::Underline),
        "a tag inside a tag is both");
  // Closing out of order takes the innermost match, and the rest survives.
  CHECK(oneParagraphIs("<u>a<mark>b</u>c</mark>d", u"abcd"), "an out-of-order close is survivable");
  return true;
}

/// The rows the phase decided: hidden, kept, dropped, literal.
bool elementsResolve() {
  CHECK(oneParagraphIs("a <span>b</span>", u"a b"), "an unwrapped tag keeps its text");
  CHECK(oneParagraphIs("a <!-- n --> b", u"a  b"), "a comment emits nothing");
  CHECK(oneParagraphIs("a <script>x</script> b", u"a  b"), "a dropped tag takes its content");
  CHECK(oneParagraphIs("a <iframe>x</iframe> b", u"a  b"), "and so does an iframe");
  CHECK(oneParagraphIs("Array<string> <T> <Vector<int>>", u"Array<string> <T> <Vector<int>>"),
        "a generic in prose is not markup");
  CHECK(oneParagraphIs("<!-- n -->", u""), "a block that is only a comment is nothing");
  CHECK(oneParagraphIs("<script>x</script>", u""), "and so is a block that is only a script");
  // An unclosed comment stays text, so one stray opener cannot hide a stream.
  CHECK(oneParagraphIs("<!-- unclosed", u"<!-- unclosed"), "an unclosed comment is text");
  // Inline HTML off puts every tag back, which is the escape hatch.
  const ParseOptions off{ParseFlags::InlineHtml & ~ParseFlags::InlineHtml};
  CHECK(parse("a <b>c</b>", off).document->blocks[0].text == u"a <b>c</b>",
        "every tag is text with the bit off");
  const ParseOptions nothing{0};
  CHECK(parse("a <b>c</b>", nothing).document->blocks[0].text == u"a <b>c</b>",
        "and with the whole option off");
  return true;
}

/// An anchor is a link, through the same policy as a Markdown one.
bool anchorsLink() {
  const ParseResult good = parse("<a href=\"https://x\">link</a>");
  CHECK(good.document->links.size() == 1, "an https anchor is a link");
  if (good.document->links.size() == 1) {
    CHECK(good.document->links[0].url == "https://x", "with its own URL");
  }
  CHECK(good.document->blocks[0].text == u"link", "and the reader sees only its text");
  CHECK(stylesOver(good, u"link") & InlineStyle::Link, "and its text is a link run");

  CHECK(parse("<a href=\"javascript:x\">y</a>").document->links.empty(), "a refused scheme is not a link");
  CHECK(parse("<a>no href</a>").document->links.empty(), "an anchor with no href is not a link");
  CHECK(parse("<a href=\"/rel\">y</a>").document->links.empty(), "a relative anchor is off by default");

  const ParseOptions relative{ParseFlags::InlineHtml | ParseFlags::LinkRelative};
  CHECK(parse("<a href=\"/rel\">y</a>", relative).document->links.size() == 1,
        "and on with the relative bit");

  // A link's range covers the whole anchor, delimiters included, so a press maps
  // back to what the writer typed.
  const ParseResult ranged = parse("see <a href=\"https://x\">link</a> ok");
  if (ranged.document->links.size() != 1) {
    return false;
  }
  const std::string source = "see <a href=\"https://x\">link</a> ok";
  const SourceRange &at = ranged.document->links[0].range;
  const std::u16string utf16 = utf8ToUtf16(source);
  CHECK(utf16.substr(at.start, at.end - at.start) == u"<a href=\"https://x\">link</a>",
        "the range covers the whole anchor");
  return true;
}

/// `allowedTags`: an app's own tag runs, and nothing else does.
bool allowedTagsRun() {
  CHECK(oneParagraphIs("a <user>bob</user> c", u"a <user>bob</user> c"), "an unallowed tag is literal");

  TagPolicy policy;
  policy.tags = {AllowedTag{"user", {}}};
  const ParseOptions with{ParseFlags::InlineHtml, policy};
  CHECK(parse("a <user>bob</user> c", with).document->blocks[0].text == u"a bob c",
        "an allowed tag is hidden and its text kept");

  // Its record names the tag, so a press and a style have something to read.
  const ParseResult tagged = parse("a <user>bob</user>", with);
  CHECK(tagged.document->tags.size() == 1, "an allowed tag is recorded");
  if (tagged.document->tags.size() == 1) {
    CHECK(tagged.document->tags[0].name == "user", "under its lowercased name");
  }
  bool named = false;
  for (const InlineRun &run : tagged.document->blocks[0].runs) {
    named = named || run.tag >= 0;
  }
  CHECK(named, "and its run names the record");

  // The wire form, which is what the prop crosses as.
  const TagPolicy wire = tagPolicyFromStrings({"-1", "user", "id", "-1", "note"});
  CHECK(wire.tags.size() == 2, "two tags came through the wire form");
  if (wire.tags.size() == 2) {
    CHECK(wire.tags[0].name == "note", "sorted by name, so the lookup is a binary search");
    CHECK(wire.tags[1].attributes.size() == 1 && wire.tags[1].attributes[0] == "id",
          "and its attributes came with it");
  }
  CHECK(tagPolicyFromStrings({"user", "id"}).tags.empty(), "a list that is not our shape is refused whole");
  CHECK(tagPolicyFromStrings({"-1"}).tags.empty(), "and so is a truncated one");
  return true;
}

} // namespace

int main() {
  CHECK(tableRows(), "the tag table");
  CHECK(stylesRun(), "the styles it defines");
  CHECK(elementsResolve(), "unwrap, hide, drop and literal");
  CHECK(anchorsLink(), "anchors");
  CHECK(allowedTagsRun(), "allowedTags");
  return ds_check_result();
}