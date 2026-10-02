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

std::optional<Fence> fenceOpener(std::string_view line) {
  const size_t start = afterIndent(line);
  if (start == kNoOffset || start >= line.size() || (line[start] != '`' && line[start] != '~')) {
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

} // namespace downstream::scan
