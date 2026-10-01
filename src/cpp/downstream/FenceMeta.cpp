#include "FenceMeta.h"

#include <algorithm>
#include <cstddef>

namespace downstream {
namespace {

constexpr char32_t kZeroWidthSpace = 0x200B;

/// The keys the renderers this covers spell a title with. Docusaurus,
/// rehype-pretty code, and Expressive Code all use `title`; Nextra uses
/// `filename`.
constexpr std::string_view kTitleKey = "title";
constexpr std::string_view kFilenameKey = "filename";
/// Streamdown's key, and the one that wins over `showLineNumbers`.
constexpr std::string_view kStartLineKey = "startLine";
/// Docusaurus, rehype-pretty code, and Nextra spell the same number these ways.
constexpr std::string_view kShowLineNumbersKey = "showLineNumbers";
/// Streamdown's key, exact and case-sensitive.
constexpr std::string_view kNoLineNumbersKey = "noLineNumbers";

bool isAsciiSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

bool isDigit(char c) {
  return c >= '0' && c <= '9';
}

/**
 * True for a unit a title must not carry: a C0 or C1 control, the zero-width
 * space, and the bidi embedding, override, and isolate characters.
 *
 * Those are what make `exe.txt` render as though it were `txt.exe`, so a title
 * is shown as a label that a model cannot repoint at something else. The
 * download name keeps its own stricter path rules on top of this.
 */
bool isStrippedUnit(char32_t unit) {
  if (unit < 0x20 || (unit >= 0x7F && unit <= 0x9F)) {
    return true;
  }
  if (unit == kZeroWidthSpace) {
    return true;
  }
  return (unit >= 0x202A && unit <= 0x202E) || (unit >= 0x2066 && unit <= 0x2069);
}

/**
 * The UTF-8 sequence at `offset` as a code point, and how many bytes it took.
 *
 * A malformed byte decodes as U+FFFD one byte at a time, so a bad sequence can
 * neither read past the end nor swallow the bytes after it.
 */
char32_t decodeUtf8(std::string_view text, size_t offset, size_t *width) {
  const auto lead = static_cast<unsigned char>(text[offset]);
  if (lead < 0x80) {
    *width = 1;
    return lead;
  }
  size_t length = 0;
  char32_t unit = 0;
  if ((lead & 0xE0) == 0xC0) {
    length = 2;
    unit = lead & 0x1Fu;
  } else if ((lead & 0xF0) == 0xE0) {
    length = 3;
    unit = lead & 0x0Fu;
  } else if ((lead & 0xF8) == 0xF0) {
    length = 4;
    unit = lead & 0x07u;
  } else {
    *width = 1;
    return 0xFFFD;
  }
  if (text.size() - offset < length) {
    *width = 1;
    return 0xFFFD;
  }
  for (size_t index = 1; index < length; index++) {
    const auto next = static_cast<unsigned char>(text[offset + index]);
    if ((next & 0xC0) != 0x80) {
      *width = 1;
      return 0xFFFD;
    }
    unit = (unit << 6) | (next & 0x3Fu);
  }
  *width = length;
  return unit;
}

/**
 * Copies `value` with every stripped unit removed and the result capped at
 * `kMaxTitleUnits` UTF-16 units.
 *
 * Returns false when nothing survives, so a title of nothing but controls is
 * no title rather than an empty label beside the language.
 */
bool appendSanitized(std::string &out, std::string_view value) {
  std::string piece;
  size_t units = 0;
  size_t offset = 0;
  while (offset < value.size() && units < kMaxTitleUnits) {
    size_t width = 0;
    const char32_t unit = decodeUtf8(value, offset, &width);
    const std::string_view bytes = value.substr(offset, width);
    offset += width;
    if (isStrippedUnit(unit)) {
      continue;
    }
    piece.append(bytes);
    // A surrogate pair is two UTF-16 units in every count that matters, which
    // is what keeps the cap from splitting one in half.
    units += unit >= 0x10000 ? 2 : 1;
  }
  if (piece.empty()) {
    return false;
  }
  out.append(piece);
  return true;
}

/**
 * One whitespace-delimited token, with a quoted run or a braced run kept whole.
 *
 * `title="a b"` is one token rather than two, and `{1,3-5}` is one token
 * rather than a brace and two numbers. `terminated` is false when a quote or a
 * brace opened and never closed, which is how a half-typed info string is told
 * from a complete one.
 */
struct Token {
  std::string_view text;
  bool terminated{true};
};

Token nextToken(std::string_view text, size_t *cursor) {
  while (*cursor < text.size() && isAsciiSpace(text[*cursor])) {
    (*cursor)++;
  }
  const size_t start = *cursor;
  while (*cursor < text.size() && !isAsciiSpace(text[*cursor])) {
    const char c = text[*cursor];
    if (c == '"' || c == '\'') {
      const size_t end = text.find(c, *cursor + 1);
      if (end == std::string_view::npos) {
        *cursor = text.size();
        return {text.substr(start, text.size() - start), false};
      }
      *cursor = end + 1;
      continue;
    }
    if (c == '{') {
      const size_t end = text.find('}', *cursor + 1);
      if (end == std::string_view::npos) {
        *cursor = text.size();
        return {text.substr(start, text.size() - start), false};
      }
      *cursor = end + 1;
      continue;
    }
    (*cursor)++;
  }
  return {text.substr(start, *cursor - start), true};
}

/**
 * A quoted value is the quoted run and nothing after it, so `title="a.ts"` and
 * `title="a.ts":src/app.ts` are one title. A value with no quotes is the rest of
 * the token.
 */
std::string_view unquote(std::string_view value) {
  if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') &&
      value.find(value.front(), 1) != std::string_view::npos) {
    return value.substr(1, value.find(value.front(), 1) - 1);
  }
  return value;
}

/// The key and the value of `key=value` or `key{value}`, the two spellings the
/// renderers in the wild use for the same number.
struct KeyValue {
  std::string_view key;
  std::string_view value;
  bool valid{false};
};

KeyValue splitKey(std::string_view token) {
  const size_t equals = token.find('=');
  if (equals != std::string_view::npos) {
    const std::string_view raw = token.substr(equals + 1);
    // `key=` with nothing after it names nothing, but `key=""` is a value that
    // happens to be empty, and the caller treats an empty title as no title.
    return {token.substr(0, equals), unquote(raw), !raw.empty()};
  }
  // `showLineNumbers{5}` is the other spelling the renderers use. The brace is
  // found rather than required at the front, because the key is always before it.
  const size_t brace = token.find('{');
  if (brace != std::string_view::npos && token.back() == '}') {
    return {token.substr(0, brace), token.substr(brace + 1, token.size() - brace - 2), true};
  }
  return {};
}

/// A line number a fence named, or 0 when these are not usable digits.
uint32_t parseStartLine(std::string_view value) {
  // Seven digits is one past `kMaxStartLine`, so anything longer is rejected
  // before it can overflow.
  if (value.empty() || value.size() > 7) {
    return 0;
  }
  uint32_t parsed = 0;
  for (const char c : value) {
    if (!isDigit(c)) {
      return 0;
    }
    parsed = parsed * 10 + static_cast<uint32_t>(c - '0');
  }
  return parsed >= 1 && parsed <= kMaxStartLine ? parsed : 0;
}

/// `{1,3-5}` as sorted, merged, inclusive ranges. Out-of-order and overlapping
/// entries combine, so two spellings of one set of lines compare equal.
std::vector<LineMark> parseMarks(std::string_view body) {
  std::vector<LineMark> marks;
  size_t offset = 0;
  while (offset < body.size() && marks.size() < kMaxLineMarks) {
    size_t end = body.find(',', offset);
    if (end == std::string_view::npos) {
      end = body.size();
    }
    const std::string_view item = body.substr(offset, end - offset);
    offset = end + 1;
    if (item.empty()) {
      continue;
    }
    const size_t dash = item.find('-');
    const uint32_t first = parseStartLine(dash == std::string_view::npos ? item : item.substr(0, dash));
    if (first == 0) {
      continue;
    }
    uint32_t last = first;
    if (dash != std::string_view::npos) {
      const uint32_t upper = parseStartLine(item.substr(dash + 1));
      // An inverted or unusable range contributes nothing rather than a range
      // the renderer would have to guess the direction of.
      if (upper == 0 || upper < first) {
        continue;
      }
      last = upper;
    }
    marks.push_back({first, last});
  }
  std::sort(marks.begin(), marks.end(), [](const LineMark &a, const LineMark &b) {
    return a.first != b.first ? a.first < b.first : a.last < b.last;
  });
  std::vector<LineMark> merged;
  for (const LineMark &mark : marks) {
    if (!merged.empty() && mark.first <= merged.back().last + 1) {
      merged.back().last = std::max(merged.back().last, mark.last);
    } else {
      merged.push_back(mark);
    }
  }
  if (merged.size() > kMaxLineMarks) {
    merged.resize(kMaxLineMarks);
  }
  return merged;
}

/// One token's effect on the meta. `startLine` is tracked apart from the field
/// so `startLine` can win over `showLineNumbers` whichever arrives first.
void readToken(FenceMeta *meta, std::string_view token, uint32_t *startLine, bool *startLineIsSpecific) {
  if (token == kNoLineNumbersKey) {
    // An exact token, so `xnoLineNumbers` and `nolineNumbers` leave numbers on.
    meta->noLineNumbers = true;
    return;
  }
  // A braced list with no key before it is `{1,3-5}`, a mark list. With a key
  // before it, `splitKey` reads the brace as that key's value instead.
  if (token.size() >= 2 && token.front() == '{' && token.back() == '}' && token.find('=') == std::string_view::npos) {
    if (meta->marks.empty()) {
      meta->marks = parseMarks(token.substr(1, token.size() - 2));
    }
    return;
  }
  const KeyValue pair = splitKey(token);
  if (!pair.valid) {
    // `showLineNumbers` on its own means nothing: numbers are already on by
    // default, and the `lineNumbers` prop still wins over any meta.
    return;
  }
  if (pair.key == kTitleKey || pair.key == kFilenameKey) {
    // The first one wins, so a later token cannot replace a name the writer put
    // first.
    if (meta->title.empty()) {
      appendSanitized(meta->title, pair.value);
    }
    return;
  }
  if (pair.key == kStartLineKey) {
    const uint32_t parsed = parseStartLine(pair.value);
    if (parsed != 0) {
      *startLine = parsed;
      *startLineIsSpecific = true;
    }
    return;
  }
  if (pair.key == kShowLineNumbersKey) {
    const uint32_t parsed = parseStartLine(pair.value);
    if (parsed != 0 && !*startLineIsSpecific) {
      *startLine = parsed;
    }
  }
}

} // namespace

FenceMeta parseFenceMeta(std::string_view info) {
  FenceMeta meta;
  if (info.empty()) {
    return meta;
  }
  // The whole string stays in `Block::info`; only the parse is bounded, because
  // an info string long enough to matter here is a model repeating itself.
  info = info.substr(0, std::min(info.size(), kMaxInfoBytes));

  // The first word is the language, which highlighting already cut at the colon.
  // Only what follows that colon in this word is a path, so a title later in the
  // string never lands here.
  size_t wordEnd = 0;
  while (wordEnd < info.size() && !isAsciiSpace(info[wordEnd])) {
    wordEnd++;
  }
  std::string pathName;
  {
    const std::string_view first = info.substr(0, wordEnd);
    const size_t colon = first.find(':');
    if (colon != std::string_view::npos) {
      pathName = std::string(first.substr(colon + 1));
    }
  }

  uint32_t startLine = 0;
  bool startLineIsSpecific = false;
  size_t cursor = wordEnd;
  while (cursor < info.size()) {
    const Token token = nextToken(info, &cursor);
    if (!token.text.empty()) {
      if (!token.terminated) {
        // An unterminated quote or brace drops this token and everything after
        // it. While the opener is still being typed the fence has no title, and
        // the header shows the language until the closing quote arrives.
        break;
      }
      readToken(&meta, token.text, &startLine, &startLineIsSpecific);
    }
  }

  if (startLine != 0) {
    meta.startLine = startLine;
  }
  // A path is the title only when nothing else named the block, which is the
  // rule the download name has always used.
  if (meta.title.empty() && !pathName.empty()) {
    appendSanitized(meta.title, pathName);
  }
  return meta;
}

} // namespace downstream
