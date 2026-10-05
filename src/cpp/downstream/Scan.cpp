#include "Scan.h"

#include <string>

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

// --- inline tag allowlist ---

namespace {

char asciiLower(char c) {
  return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
}

bool isTagNameChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool isTagNameEnd(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '/' || c == '>';
}

} // namespace

InlineTag matchInlineTag(std::string_view piece) {
  if (piece.size() < 3 || piece[0] != '<') {
    return InlineTag::None;
  }
  size_t i = 1;
  const bool closing = piece[i] == '/';
  if (closing) {
    i++;
  }
  const size_t nameStart = i;
  while (i < piece.size() && isTagNameChar(piece[i])) {
    i++;
  }
  const size_t nameEnd = i;
  if (nameEnd == nameStart) {
    return InlineTag::None;
  }
  if (i >= piece.size() || !isTagNameEnd(piece[i])) {
    // `<subtitle>` and friends: the name runs past `sub`, so this is not a
    // match, and a prefix is never accepted.
    return InlineTag::None;
  }
  std::string name;
  for (size_t k = nameStart; k < nameEnd; k++) {
    name.push_back(asciiLower(piece[k]));
  }
  if (name == "br") {
    // A stray `</br>` is a break, because HTML parsers read it that way and
    // models write it.
    return InlineTag::Break;
  }
  if (name == "sub" || name == "sup") {
    // Self-closing is a slash before the closing `>`, which may carry trailing
    // space: `<sub/>` and `<sup />` are both written.
    const std::string_view rest = piece.substr(i);
    size_t end = rest.size();
    while (end > 0 && rest[end - 1] == ' ') {
      end--;
    }
    const bool selfClosing = end > 1 && rest[end - 1] == '>' && rest[end - 2] == '/';
    if (name == "sub") {
      return closing ? InlineTag::SubClose : (selfClosing ? InlineTag::SubVoid : InlineTag::SubOpen);
    }
    return closing ? InlineTag::SupClose : (selfClosing ? InlineTag::SupVoid : InlineTag::SupOpen);
  }
  return InlineTag::None;
}

// A strict prefix of any allowed tag. Names are the three tags', so a name
// letter sequence that can no longer extend to one of them ends the prefix.
bool isInlineTagPrefix(std::string_view fragment) {
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
  size_t i = 1;
  if (i >= fragment.size()) {
    // The bare `<`, with nothing after it yet.
    return true;
  }
  if (fragment[i] == '/') {
    i++;
    if (i >= fragment.size() || !isTagNameChar(fragment[i])) {
      return false;
    }
  } else if (!isTagNameChar(fragment[i])) {
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
        next != '"' && next != '\'') {
      return false;
    }
  }
  // The prefix letters so far must still be able to complete to br, sub or sup.
  // Anything else, including a longer word that starts like one of them, is
  // already prose.
  std::string letters;
  for (size_t k = nameStart; k < i; k++) {
    letters.push_back(asciiLower(fragment[k]));
  }
  const std::string_view names[] = {"br", "sub", "sup"};
  for (const std::string_view candidate : names) {
    if (candidate.substr(0, letters.size()) == letters) {
      return true;
    }
  }
  return false;
}

} // namespace downstream::scan
