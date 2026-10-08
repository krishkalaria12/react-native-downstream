// The HTML subset, through the real parser.
//
// `Scan.cpp`'s table and `MarkdownParser.cpp`'s application of it are one
// decision split in two, and neither half proves the other: a table can classify
// a tag correctly while the builder ignores it. Every case here goes through
// `parseMarkdown`, so a row that the builder never reached would be caught.

#include "MarkdownParser.h"
#include "Scan.h"
#include "TagSlots.h"
#include "Unicode.h"
#include "check.h"

#include <string>
#include <string_view>
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

/// `literalTagContent`: an allowed tag's content is raw source, not Markdown.
bool literalTagContentRun() {
  TagPolicy policy;
  policy.tags = {AllowedTag{"user", {}}};
  const ParseOptions plain{ParseFlags::InlineHtml, policy};
  const ParseOptions literal{ParseFlags::InlineHtml | ParseFlags::LiteralTagContent, policy};

  // Off by default: the emphasis inside is emphasis.
  CHECK(stylesOver(parse("a <user>**bob**</user>", plain), u"bob") == InlineStyle::Strong,
        "without the flag the content is parsed");

  // On: the content is not interpreted — no style, though md4c still owns
  // the markers themselves, so they read as syntax rather than text.
  const ParseResult flat = parse("a <user>**bob**</user> c", literal);
  CHECK(flat.document->blocks[0].text == u"a bob c", "the text is kept");
  CHECK(stylesOver(flat, u"bob") == 0, "but unstyled");

  // The tag itself is still real: recorded, and its runs name it.
  CHECK(flat.document->tags.size() == 1, "the tag is still recorded");
  bool named = false;
  for (const InlineRun &run : flat.document->blocks[0].runs) {
    named = named || run.tag >= 0;
  }
  CHECK(named, "and its runs name the record");

  // Links do not form inside either: no record, no style.
  const ParseResult linked = parse("<user>[x](https://e.com)</user>", literal);
  CHECK(linked.document->links.empty(), "no link is recorded");
  CHECK(stylesOver(linked, u"x") == 0, "and none is styled");

  // Builtin tags inside are raw source too, consumed rather than applied.
  const ParseResult builtin = parse("<user><b>x</b></user>", literal);
  CHECK(builtin.document->blocks[0].text == u"x", "the text is kept");
  CHECK(stylesOver(builtin, u"x") == 0, "but the bold is not applied");

  // A nested allowed tag is a second record, and its content is literal too.
  const ParseResult nested = parse("<user>a<user>b</user>c</user>", literal);
  CHECK(nested.document->tags.size() == 2, "both tags are recorded");
  CHECK(nested.document->blocks[0].text == u"abc", "with literal text throughout");
  CHECK(stylesOver(nested, u"abc") == 0, "and no style anywhere");

  // An unclosed tag ends with its block: the next paragraph parses normally.
  const ParseResult open = parse("<user>**bob**\n\n**hi**", literal);
  CHECK(open.document->blocks.size() == 2, "two paragraphs");
  if (open.document->blocks.size() == 2) {
    CHECK(stylesOver(open, u"bob") == 0, "the open tag's content is literal");
    const std::u16string &second = open.document->blocks[1].text;
    const size_t at = second.find(u"hi");
    uint32_t bits = 0;
    for (const InlineRun &run : open.document->blocks[1].runs) {
      if (run.start < at + 2 && run.end > at) {
        bits |= run.styles;
      }
    }
    CHECK(bits == InlineStyle::Strong, "but the next block parses");
  }
  return true;
}

void hostedTagRanges() {
  TagPolicy policy;
  policy.tags = {AllowedTag{"user", {}}};
  const ParseOptions options{ParseFlags::Default | ParseFlags::InlineHtml, policy};
  const auto slots = [&](const std::string &source) {
    const auto result = parse(source, options);
    CHECK(result.status == ParseStatus::Ok, "the hosted tag fixture parses");
    return tagSlots(*result.document, source, {"user"}, false,
                    static_cast<uint32_t>(result.document->blocks.size()));
  };
  const auto simple = slots("Before.\n\n<user>\n\none\n\ntwo\n\n</user>\n\nAfter.\n");
  CHECK(simple.size() == 1 && simple[0].blockIndex == 1 && simple[0].blockCount == 2,
        "one complete custom segment remains hostable after prose");
  CHECK(slots("<user>\n\none\n\n```js\nx\n```\n\ntwo\n\n</user>\n").empty(),
        "a tag spanning text and code falls back to native rendering");
  CHECK(slots("<user>\n\n| x |\n| - |\n| y |\n\n</user>\n").empty(),
        "a table-only tag has no custom segment to host");
  CHECK(slots("<user>\n\nBefore.\n\n<details>\n<summary>A</summary>\n\none\n\n</details>\n\nAfter.\n\n</user>\n").empty(),
        "a tag spanning details visibility boundaries falls back to native rendering");
  const auto nested = slots("<user>\n\nOuter before.\n\n<user>\n\nInner.\n\n</user>\n\nOuter after.\n\n</user>\n");
  CHECK(nested.size() == 1 && nested[0].markdown == "Inner.",
        "a nested tag can be hosted without partially hosting its outer tag");
}

void blockTagAttributes() {
  TagPolicy policy;
  policy.tags = {AllowedTag{"card", {"id"}}};
  const ParseOptions options{ParseFlags::Default | ParseFlags::InlineHtml, policy};
  const std::string source = "Before.\n\n<card id=\"safe\" onclick=\"bad\">\n\nBody.\n\n</card>\n";
  const auto result = parse(source, options);
  CHECK(result.status == ParseStatus::Ok, "the attributed block tag parses");
  const std::vector<std::pair<std::string, std::string>> expected{{"id", "safe"}};
  CHECK(result.document->tags.size() == 1 && result.document->tags[0].attributes == expected,
        "a block tag preserves only attributes allowed by its policy");
  const auto slots = tagSlots(*result.document, source, {"card"}, false,
                             static_cast<uint32_t>(result.document->blocks.size()));
  CHECK(slots.size() == 1 && slots[0].attributes == expected,
        "the hosted block slot receives the permitted attribute and excludes onclick");
  for (const std::string_view opener : {
           "<card data_id=\"x\" id=\"safe\">",
           "<card $id=\"bad\" id=\"safe\">",
           "<card _id=\"bad id=evil\" id=\"safe\">",
           "<card data_id id=\"safe\">"}) {
    CHECK(classifyTag(opener, policy).attributes == expected,
          "unsupported attribute names are skipped whole without losing the next permitted attribute");
  }
}

void detailsSourceRanges() {
  std::string source = "😀 Préface [first](https://first.example).\n\n";
  for (size_t index = 0; index < 40; ++index) {
    source += "<details><summary>世界 😀</summary>é [link](https://example.com).</details>\n\n";
  }
  const auto result = parse(source);
  CHECK(result.status == ParseStatus::Ok && result.document->links.size() == 41,
        "sibling details retain every link after Unicode prose");
  size_t from = 0;
  for (const auto &link : result.document->links) {
    const size_t start = source.find('[', from);
    const size_t end = source.find(')', start) + 1;
    CHECK(link.range.start == utf16Length(std::string_view(source).substr(0, start)) &&
              link.range.end == utf16Length(std::string_view(source).substr(0, end)),
          "details sub-parse ranges retain absolute UTF-16 offsets across siblings");
    from = end;
  }
}

} // namespace

int main() {
  CHECK(tableRows(), "the tag table");
  CHECK(stylesRun(), "the styles it defines");
  CHECK(elementsResolve(), "unwrap, hide, drop and literal");
  CHECK(anchorsLink(), "anchors");
  CHECK(allowedTagsRun(), "allowedTags");
  CHECK(literalTagContentRun(), "literalTagContent");
  hostedTagRanges();
  blockTagAttributes();
  detailsSourceRanges();
  return ds_check_result();
}
