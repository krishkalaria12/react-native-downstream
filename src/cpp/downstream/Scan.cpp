#include "Scan.h"

#include <algorithm>
#include <string>
#include <vector>

namespace downstream::scan {

bool isEscaped(std::string_view source, size_t index) {
  size_t backslashes = 0;
  while (index > backslashes && source[index - backslashes - 1] == '\\') {
    backslashes++;
  }
  return backslashes % 2 == 1;
}

size_t findUnescaped(std::string_view source, char needle, size_t from) {
  for (size_t i = from; i < source.size(); i++) {
    if (source[i] == needle && !isEscaped(source, i)) {
      return i;
    }
  }
  return kNoOffset;
}

bool isBlankLine(std::string_view line) {
  return line.find_first_not_of(" \t\r") == kNoOffset;
}

size_t afterIndent(std::string_view line) {
  size_t index = 0;
  while (index < 3 && index < line.size() && line[index] == ' ') {
    index++;
  }
  return index < line.size() && line[index] == ' ' ? kNoOffset : index;
}

bool startsWith(std::string_view line, char marker) {
  const size_t start = afterIndent(line);
  return start != kNoOffset && start < line.size() && line[start] == marker;
}

bool isMarkdownSpace(char16_t unit) {
  return unit == u' ' || unit == u'\t' || unit == u'\n' || unit == u'\r' || unit == 0x0B || unit == 0x0C;
}

bool isWordish(char16_t unit) {
  if (unit < 0x80) {
    return (unit >= u'a' && unit <= u'z') || (unit >= u'A' && unit <= u'Z') ||
        (unit >= u'0' && unit <= u'9') || unit == u'_';
  }
  return true;
}

std::optional<Fence> fenceOpener(std::string_view line, bool mathFences) {
  const size_t start = afterIndent(line);
  if (start == kNoOffset || start >= line.size()) {
    return std::nullopt;
  }
  if (mathFences && line[start] == '$') {
    // The same shape md4c's patched fence check has, so the two agree on which
    // line opens and which closes. A disagreement here would let a commit land
    // between them.
    if (isMathFenceLine(line)) {
      return Fence{'$', 2, start, std::string_view()};
    }
    return std::nullopt;
  }
  if (line[start] != '`' && line[start] != '~') {
    return std::nullopt;
  }
  const char marker = line[start];
  size_t end = start;
  while (end < line.size() && line[end] == marker) {
    end++;
  }
  const std::string_view info = line.substr(end);
  if (end - start < 3 || (marker == '`' && info.find('`') != kNoOffset)) {
    return std::nullopt;
  }
  return Fence{marker, end - start, start, info};
}

bool closesFence(std::string_view line, char marker, size_t length) {
  const size_t start = afterIndent(line);
  if (start == kNoOffset) {
    return false;
  }
  size_t end = start;
  while (end < line.size() && line[end] == marker) {
    end++;
  }
  return end - start >= length && isBlankLine(line.substr(end));
}

bool isMathFenceLine(std::string_view line) {
  const size_t start = afterIndent(line);
  // Exactly two dollars, and nothing after them but whitespace. A run of three
  // is a code span, and a `$$ x` is prose rather than a fence.
  return start != kNoOffset && line.size() >= start + 2 && line[start] == '$' && line[start + 1] == '$' &&
      isBlankLine(line.substr(start + 2));
}

bool isFenceLine(std::string_view line) {
  const size_t start = afterIndent(line);
  return start != kNoOffset && start < line.size() && (line[start] == '`' || line[start] == '~') &&
      closesFence(line, line[start], 3);
}

/// Offset just past a line's blockquote markers and one list marker, or
/// `afterIndent` when the line opens neither.
///
/// A fenced block nested in a quote or a list item is still a fenced block, so
/// the fence scan has to see past the container that holds it. Only one marker is
/// stripped per line, which is what the incremental parser's own fence scan does
/// not need: it walks top-level fences only, and this rule is about the last
/// block of a document, where nesting is shallow.
size_t afterContainers(std::string_view line) {
  size_t at = afterIndent(line);
  if (at == kNoOffset) {
    return kNoOffset;
  }
  // A quote marker may repeat, and each one may be followed by one space.
  for (;;) {
    if (at < line.size() && line[at] == '>') {
      at++;
      if (at < line.size() && line[at] == ' ') {
        at++;
      }
      continue;
    }
    break;
  }
  // A bullet, or a number with a delimiter. Either needs a space after it, or it
  // is ordinary text that happens to start with a dash.
  size_t end = at;
  if (end < line.size() && (line[end] == '-' || line[end] == '+' || line[end] == '*')) {
    end++;
  } else {
    size_t digits = end;
    while (digits < line.size() && digits - end < 9 && line[digits] >= '0' && line[digits] <= '9') {
      digits++;
    }
    if (digits == end || digits >= line.size() || (line[digits] != '.' && line[digits] != ')')) {
      return at;
    }
    end = digits + 1;
  }
  if (end >= line.size() || (line[end] != ' ' && line[end] != '\t')) {
    return at;
  }
  while (end < line.size() && (line[end] == ' ' || line[end] == '\t')) {
    end++;
  }
  return end;
}

/// True when this line closes a fence opened with `marker` repeated `length`
/// times, looking past any container that holds it.
bool closesNestedFence(std::string_view line, char marker, size_t length) {
  const size_t start = afterContainers(line);
  if (start == kNoOffset || start >= line.size() || line[start] != marker) {
    return false;
  }
  size_t end = start;
  while (end < line.size() && line[end] == marker) {
    end++;
  }
  return end - start >= length && isBlankLine(line.substr(end));
}

bool endsInsideFence(std::string_view source) {
  char marker = 0;
  size_t length = 0;
  for (size_t at = 0; at <= source.size();) {
    const size_t newline = source.find('\n', at);
    const size_t end = newline == kNoOffset ? source.size() : newline;
    const std::string_view line = source.substr(at, end - at);
    if (marker != 0) {
      if (closesNestedFence(line, marker, length)) {
        marker = 0;
      }
    } else {
      const size_t start = afterContainers(line);
      if (start != kNoOffset && start < line.size() && (line[start] == '`' || line[start] == '~')) {
        size_t run = start;
        while (run < line.size() && line[run] == line[start]) {
          run++;
        }
        const std::string_view info = line.substr(run);
        // Three markers at least, and a backtick fence takes no backtick in its
        // info string, which is what keeps a fence line inside code from opening
        // one.
        if (run - start >= 3 && !(line[start] == '`' && info.find('`') != kNoOffset)) {
          marker = line[start];
          length = run - start;
        }
      }
    }
    if (newline == kNoOffset) {
      break;
    }
    at = newline + 1;
  }
  return marker != 0;
}

size_t skipInlineDestination(std::string_view source, size_t open) {
  size_t depth = 0;
  char quote = 0;
  for (size_t i = open; i < source.size(); i++) {
    const char c = source[i];
    if (c == '\\') {
      i++;
      continue;
    }
    if (quote != 0) {
      if (c == quote) {
        quote = 0;
      }
      continue;
    }
    if (c == '<' && depth == 1) {
      const size_t close = findUnescaped(source, '>', i + 1);
      if (close == kNoOffset) {
        return kNoOffset;
      }
      i = close;
    } else if (c == '"' || c == '\'') {
      quote = c;
    } else if (c == '(') {
      depth++;
    } else if (c == ')') {
      if (--depth == 0) {
        return i + 1;
      }
    }
  }
  return kNoOffset;
}

bool isUnfinishedDestination(std::string_view source, size_t at) {
  return at >= 2 && source[at - 1] == '(' && source[at - 2] == ']' && !isEscaped(source, at - 2);
}

// --- HTML tags ---

namespace {

char asciiLower(char c) {
  return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
}

/// Lowercases an ASCII range into `out`. HTML element and attribute names are
/// ASCII, and a name with a byte above 0x7f is not a name.
std::string lowered(std::string_view text) {
  std::string out(text);
  for (char &c : out) {
    c = asciiLower(c);
  }
  return out;
}

/**
 * One row of the tag table.
 *
 * A name, and what to do with it. `closingStyle` is what the *end* tag does
 * where that differs — `</b>` closes a strong span the way `<b>` opened one,
 * while `</br>` is a break and `</sub>` is a stray closer a browser swallows.
 */
struct TagRow {
  std::string_view name;
  TagAction action;
  TagStyle style;
  /// What the end tag does. `TagStyle::None` means "the same, reversed".
  TagStyle closingStyle;
};

/// Sorted by name, which is what makes the lookup a binary search.
///
/// The order is load-bearing: `findRow` is a bisection and `isInlineTagPrefix`
/// walks it, so a row added out of order would make both silently wrong. The
/// three groups the phase table describes — the styles that already existed,
/// the styles phase 4 added, and the elements with no style of ours — are
/// interleaved here because a table sorted by meaning is not a table that can be
/// searched.
constexpr TagRow TAG_TABLE[] = {
    {"a", TagAction::Link, TagStyle::None, TagStyle::None}, {"abbr", TagAction::Unwrap, TagStyle::None, TagStyle::None}, {"b", TagAction::Style, TagStyle::Strong, TagStyle::None},
    {"br", TagAction::Style, TagStyle::Break, TagStyle::Break}, {"code", TagAction::Style, TagStyle::Code, TagStyle::None}, {"del", TagAction::Style, TagStyle::Strikethrough, TagStyle::None},
    {"em", TagAction::Style, TagStyle::Emphasis, TagStyle::None}, {"embed", TagAction::DropContent, TagStyle::None, TagStyle::None}, {"font", TagAction::Unwrap, TagStyle::None, TagStyle::None},
    {"i", TagAction::Style, TagStyle::Emphasis, TagStyle::None}, {"iframe", TagAction::DropContent, TagStyle::None, TagStyle::None}, {"ins", TagAction::Style, TagStyle::Underline, TagStyle::None},
    {"kbd", TagAction::Style, TagStyle::Keyboard, TagStyle::None}, {"mark", TagAction::Style, TagStyle::Highlight, TagStyle::None}, {"object", TagAction::DropContent, TagStyle::None, TagStyle::None},
    {"q", TagAction::Unwrap, TagStyle::None, TagStyle::None}, {"s", TagAction::Style, TagStyle::Strikethrough, TagStyle::None}, {"samp", TagAction::Style, TagStyle::Code, TagStyle::None},
    {"script", TagAction::DropContent, TagStyle::None, TagStyle::None}, {"small", TagAction::Unwrap, TagStyle::None, TagStyle::None}, {"span", TagAction::Unwrap, TagStyle::None, TagStyle::None},
    {"strike", TagAction::Style, TagStyle::Strikethrough, TagStyle::None}, {"strong", TagAction::Style, TagStyle::Strong, TagStyle::None}, {"style", TagAction::DropContent, TagStyle::None, TagStyle::None},
    {"sub", TagAction::Style, TagStyle::Subscript, TagStyle::Subscript}, {"sup", TagAction::Style, TagStyle::Superscript, TagStyle::Superscript}, {"template", TagAction::DropContent, TagStyle::None, TagStyle::None},
    {"time", TagAction::Unwrap, TagStyle::None, TagStyle::None}, {"tt", TagAction::Style, TagStyle::Code, TagStyle::None}, {"u", TagAction::Style, TagStyle::Underline, TagStyle::None},
};

constexpr size_t TAG_ROWS = sizeof(TAG_TABLE) / sizeof(TagRow);

const TagRow *findRow(std::string_view name) {
  size_t lo = 0;
  size_t hi = TAG_ROWS;
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    const std::string_view candidate = TAG_TABLE[mid].name;
    if (name < candidate) {
      hi = mid;
    } else if (name > candidate) {
      lo = mid + 1;
    } else {
      return &TAG_TABLE[mid];
    }
  }
  return nullptr;
}

/**
 * A name that is an HTML element, whether or not this table draws it.
 *
 * The distinction matters for one decision only. `Array<string>` is a generic,
 * not an element, so it keeps its text; `<span>` is an element the table has no
 * style for, so it is unwrapped. A table of "the names this renderer draws"
 * cannot answer that question, because it is a question about HTML rather than
 * about this renderer — which is why this list is separate and why it is
 * short: the elements a model actually emits in prose.
 */
bool isHtmlElementName(std::string_view name) {
  // The full HTML element list is 110-odd names. A model writing prose emits a
  // handful of them, and a name not on this list is unwrapped only if the app
  // asked it to be. What this list is for is not completeness: it is so that
  // `<T>`, `<Vector<string>>` and `<my-widget>` are never mistaken for markup.
  static constexpr std::string_view ELEMENTS[] = {
      "a",        "abbr",     "address",   "area",     "article",  "aside",
      "audio",    "b",        "base",      "bdi",      "bdo",      "blockquote",
      "body",     "br",       "button",    "canvas",   "caption",  "cite",
      "code",     "col",      "colgroup",  "data",     "datalist", "dd",
      "del",      "details",  "dfn",       "dialog",   "div",      "dl",
      "dt",       "em",       "embed",     "fieldset", "figcaption", "figure",
      "footer",   "form",     "h1",        "h2",       "h3",       "h4",
      "h5",       "h6",       "head",      "header",   "hgroup",   "hr",
      "html",     "i",        "iframe",    "img",      "input",    "ins",
      "kbd",      "label",    "legend",    "li",       "link",     "main",
      "map",      "mark",     "menu",      "meta",     "meter",    "nav",
      "noscript", "object",   "ol",        "optgroup", "option",   "output",
      "p",        "param",    "picture",   "pre",      "progress", "q",
      "rp",       "rt",       "ruby",      "s",        "samp",     "script",
      "section",  "select",   "slot",      "small",    "source",   "span",
      "strong",   "style",    "sub",       "summary",  "sup",      "svg",
      "table",    "tbody",    "td",        "template", "textarea", "tfoot",
      "th",       "thead",    "time",      "title",    "tr",       "track",
      "u",        "ul",       "var",       "video",    "wbr",
  };
  // `ELEMENTS` is not sorted by hand, so this is a linear scan. It runs once per
  // tag piece and the list is short; a sorted copy would be a second list to
  // keep right, and this one is the one a reader looks at when asking what
  // counts as an element.
  for (const std::string_view element : ELEMENTS) {
    if (element == name) {
      return true;
    }
  }
  return false;
}

/// Splits `<name attrs>` into its lowercased name and the rest.
struct TagHead {
  std::string name;
  std::string_view rest;
  bool closing{false};
  /// A trailing `/` before the `>`, which is `selfClosing`.
  bool selfClosing{false};
  bool valid{false};
};

/// A tag name runs letters, digits and hyphens, so `<my-widget>` is one name
/// and an app may allow it. A name that starts with a digit is not an element
/// name, and `<3` is not a tag at all.
bool isTagNameStart(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool isTagNameChar(char c) {
  return isTagNameStart(c) || (c >= '0' && c <= '9') || c == '-';
}

bool isTagNameEnd(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '/' || c == '>' ||
         c == '=';
}

TagHead parseTagHead(std::string_view piece) {
  TagHead head;
  if (piece.size() < 3 || piece.front() != '<') {
    return head;
  }
  size_t i = 1;
  if (piece[i] == '/') {
    head.closing = true;
    i++;
  }
  if (i >= piece.size() || !isTagNameStart(piece[i])) {
    return head;
  }
  const size_t nameStart = i;
  while (i < piece.size() && isTagNameChar(piece[i])) {
    i++;
  }
  head.name = lowered(piece.substr(nameStart, i - nameStart));
  head.rest = piece.substr(i);
  if (i < piece.size() && !isTagNameEnd(piece[i])) {
    // The name runs into something that cannot be in a name, so this is not a
    // tag: `<sub:title>` and `<a=b>` are prose.
    return head;
  }
  // `<sub/>` and `<sub />` are both written; `<br clear="all">` is not void.
  std::string_view rest = head.rest;
  while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\t')) {
    rest.remove_prefix(1);
  }
  if (rest.size() >= 2 && rest.front() == '/' && rest.back() == '>') {
    head.selfClosing = true;
  }
  head.valid = true;
  return head;
}

/**
 * One `href` value, from `<a href="…">` and `<a href=…>`.
 *
 * Quoted and unquoted, with entities decoded by the caller. An unquoted value
 * ends at whitespace or at the `>`, which is the HTML rule, so
 * `<a href=/docs/a>` works and `<a href=/docs/a title=x>` does not swallow the
 * next attribute.
 */
std::string_view readHref(std::string_view rest) {
  size_t i = 0;
  while (i < rest.size()) {
    while (i < rest.size() && (rest[i] == ' ' || rest[i] == '\t' || rest[i] == '\n')) {
      i++;
    }
    const size_t nameStart = i;
    while (i < rest.size() && isTagNameChar(rest[i])) {
      i++;
    }
    if (i == nameStart) {
      return {};
    }
    const std::string name = lowered(rest.substr(nameStart, i - nameStart));
    while (i < rest.size() && (rest[i] == ' ' || rest[i] == '\t')) {
      i++;
    }
    if (i >= rest.size() || rest[i] != '=') {
      // A valueless attribute; keep looking.
      continue;
    }
    i++;
    while (i < rest.size() && (rest[i] == ' ' || rest[i] == '\t')) {
      i++;
    }
    if (i >= rest.size()) {
      return {};
    }
    const char quote = rest[i];
    if (quote == '"' || quote == '\'') {
      const size_t valueStart = ++i;
      const size_t close = rest.find(quote, i);
      if (close == std::string_view::npos) {
        return {};
      }
      if (name != "href") {
        i = close + 1;
        continue;
      }
      return rest.substr(valueStart, close - valueStart);
    }
    const size_t valueStart = i;
    while (i < rest.size() && rest[i] != '>' && rest[i] != ' ' && rest[i] != '\t') {
      i++;
    }
    if (name != "href") {
      continue;
    }
    return rest.substr(valueStart, i - valueStart);
  }
  return {};
}

/**
 * The attributes in `allowed` that this tag actually carries, as written.
 *
 * An attribute the policy did not name is dropped, which is the point of naming
 * them: `<user onclick="…">` reaches a text segment as `<user>` and nothing else.
 * `href` is read the same way when it is on the list, so `id` and `href` come
 * from one loop and one rule.
 */
std::vector<std::pair<std::string, std::string>>
readAllowedAttributes(std::string_view rest, const std::vector<std::string> &allowed) {
  std::vector<std::pair<std::string, std::string>> out;
  size_t i = 0;
  while (i < rest.size()) {
    while (i < rest.size() && (rest[i] == ' ' || rest[i] == '\t' || rest[i] == '\n')) {
      i++;
    }
    if (i >= rest.size() || rest[i] == '/' || rest[i] == '>') {
      break;
    }
    const size_t nameStart = i;
    while (i < rest.size() && isTagNameChar(rest[i])) {
      i++;
    }
    const std::string name = lowered(rest.substr(nameStart, i - nameStart));
    while (i < rest.size() && (rest[i] == ' ' || rest[i] == '\t')) {
      i++;
    }
    if (i >= rest.size() || rest[i] != '=') {
      // A valueless attribute, which is not one any of these tags carry.
      continue;
    }
    i++;
    while (i < rest.size() && (rest[i] == ' ' || rest[i] == '\t')) {
      i++;
    }
    if (i >= rest.size()) {
      break;
    }
    std::string_view value;
    const char quote = rest[i];
    if (quote == '"' || quote == '\'') {
      const size_t valueStart = ++i;
      const size_t close = rest.find(quote, i);
      if (close == std::string_view::npos) {
        break;
      }
      value = rest.substr(valueStart, close - valueStart);
      i = close + 1;
    } else {
      const size_t valueStart = i;
      while (i < rest.size() && rest[i] != '>' && rest[i] != ' ' && rest[i] != '\t') {
        i++;
      }
      value = rest.substr(valueStart, i - valueStart);
    }
    if (std::find(allowed.begin(), allowed.end(), name) != allowed.end()) {
      out.emplace_back(name, std::string(value));
    }
  }
  return out;
}

} // namespace

TagPiece classifyTag(std::string_view piece, const TagPolicy &policy) {
  TagPiece out;

  // A comment is a comment before anything else, so `<-- x` is not a tag and a
  // comment is never a literal.
  if (piece.size() >= 4 && piece.compare(0, 4, "<!--") == 0) {
    out.action = TagAction::Hide;
    out.comment = true;
    out.closing = piece.size() > 5 && piece[4] == '-';
    out.name = "!--";
    return out;
  }

  const TagHead head = parseTagHead(piece);
  if (!head.valid) {
    return out;
  }
  out.name = head.name;

  const TagRow *row = findRow(head.name);
  if (row == nullptr) {
    // Not a row of the built-in table. An app's own tag is looked for in the
    // policy, and only then in the element list.
    if (!policy.tags.empty()) {
      const auto entry =
          std::lower_bound(policy.tags.begin(), policy.tags.end(), head.name,
                           [](const AllowedTag &tag, std::string_view name) { return tag.name < name; });
      if (entry != policy.tags.end() && entry->name == head.name) {
        out.action = TagAction::Style;
        out.style = TagStyle::None;
        out.tag = static_cast<int32_t>(entry - policy.tags.begin());
        out.closing = head.closing;
        out.selfClosing = head.selfClosing;
        if (!head.closing) {
          out.href = readHref(head.rest);
          out.attributes = readAllowedAttributes(head.rest, entry->attributes);
        }
        return out;
      }
    }
    // A name that is not an HTML element is a generic in prose, and a generic
    // keeps its text: `Array<string>` and `<T>` are code, not markup.
    if (!policy.unwrapUnknownElements || !isHtmlElementName(head.name)) {
      return out;
    }
    // Streamdown's reading for a real element: unwrapped, so its text survives
    // and the tag does not.
    out.action = TagAction::Unwrap;
    return out;
  }

  out.name = head.name;
  out.action = row->action;
  // A self-closed script opens and closes nothing, which is a decision about
  // the trailing slash rather than about the name.
  if (head.selfClosing && row->style != TagStyle::Break) {
    out.style = TagStyle::VoidScript;
  } else {
    // `closingStyle` is `None` for "the same style, reversed": `</b>` closes
    // what `<b>` opened, and only `br` and the two scripts differ.
    out.style = head.closing && row->closingStyle != TagStyle::None ? row->closingStyle : row->style;
  }
  out.closing = head.closing;
  out.selfClosing = head.selfClosing;
  if (row->action == TagAction::Link && !head.closing) {
    out.href = readHref(head.rest);
  }
  return out;
}

// A strict prefix of any name the table acts on. Names are the table's, so a
// name letter sequence that can no longer extend to one of them ends the prefix.
bool isInlineTagPrefix(std::string_view fragment, const TagPolicy &policy) {
  // A bare `<` is a fragment: it is the most common tail a stream stops on, and
  // the caller's proof is what decides whether it was a tag after all.
  if (fragment.empty() || fragment[0] != '<') {
    return false;
  }
  // ASCII only, because the caller's proof compares the fragment's bytes with
  // the block text's UTF-16 units and the two are only the same number here.
  for (const char c : fragment) {
    if (static_cast<unsigned char>(c) >= 0x80) {
      return false;
    }
  }
  // A comment is a prefix too: `<!--` is the commonest tail of all, and hiding
  // half of one is the same decision as hiding half a tag. Checked before the
  // name, because `!` is not a name character and would end the scan early.
  if (fragment.compare(0, 4, "<!--") == 0) {
    return true;
  }
  size_t i = 1;
  if (i >= fragment.size()) {
    // The bare `<`, with nothing after it yet.
    return true;
  }
  if (fragment[i] == '/') {
    i++;
    if (i >= fragment.size() || !isTagNameStart(fragment[i])) {
      return false;
    }
  } else if (!isTagNameStart(fragment[i])) {
    return false;
  }
  if (fragment.find('>') != std::string_view::npos) {
    return false;
  }
  const size_t nameStart = i;
  while (i < fragment.size() && isTagNameChar(fragment[i])) {
    i++;
  }
  // The remaining fragment past the name must be a plausible continuation:
  // empty (the name itself may still be short), or a delimiter start.
  if (i < fragment.size()) {
    const char next = fragment[i];
    if (next != ' ' && next != '\t' && next != '\n' && next != '\r' && next != '/' &&
        next != '"' && next != '\'' && next != '=') {
      return false;
    }
  }
  // The prefix letters so far must still be able to complete to a name the
  // table acts on. Anything else, including a longer word that starts like one
  // of them, is already prose.
  const std::string partial = lowered(fragment.substr(nameStart, i - nameStart));
  if (findRow(partial) != nullptr) {
    return true;
  }
  for (size_t row = 0; row < TAG_ROWS; row++) {
    const std::string_view name = TAG_TABLE[row].name;
    if (partial.size() <= name.size() &&
        std::equal(partial.begin(), partial.end(), name.begin(),
                   [](char actual, char expected) { return actual == expected; })) {
      return true;
    }
  }
  for (const AllowedTag &tag : policy.tags) {
    if (partial.size() <= tag.name.size() &&
        std::equal(partial.begin(), partial.end(), tag.name.begin(),
                   [](char actual, char expected) { return actual == expected; })) {
      return true;
    }
  }
  return false;
}

} // namespace downstream::scan
