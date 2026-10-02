#include "Scan.h"

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

} // namespace downstream::scan
