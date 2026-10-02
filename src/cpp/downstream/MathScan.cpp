#include "MathScan.h"

#include "Scan.h"

namespace downstream::scan {

namespace {

/// True when the byte at `at` is a backtick run, and reports its length.
/// Length 0 for anything else, so one call answers both questions.
size_t backtickRun(std::string_view source, size_t at) {
  size_t end = at;
  while (end < source.size() && source[end] == '`') {
    end++;
  }
  return end - at;
}

/**
 * Whether `at` starts a code span, and if so where the span ends.
 *
 * A code span's contents are not examined for anything, so finding one is how
 * the scan skips it. An unclosed run is not a span at all — CommonMark needs a
 * closer — so a `$` after an unclosed backtick is text that the parser will
 * report, and this returns false.
 */
std::optional<size_t> codeSpanEnd(std::string_view source, size_t at) {
  const size_t run = backtickRun(source, at);
  if (run == 0) {
    return std::nullopt;
  }
  size_t search = at + run;
  while (search < source.size()) {
    if (source[search] == '`') {
      const size_t closer = backtickRun(source, search);
      if (closer == run) {
        return search + closer;
      }
      search += closer;
      continue;
    }
    search++;
  }
  return std::nullopt;
}

/// The line `at` sits in, without its newline.
std::string_view lineAt(std::string_view source, size_t at) {
  size_t start = source.rfind('\n', at == 0 ? 0 : at - 1);
  start = (start == std::string_view::npos || at == 0) ? 0 : start + 1;
  size_t end = source.find('\n', at);
  if (end == std::string_view::npos) {
    end = source.size();
  }
  return source.substr(start, end - start);
}

/// True when a line holding only this two-byte delimiter (and whitespace) is a
/// fence line rather than a span. `\[` alone on a line has to become a `$$` line
/// for a multi-line formula to survive a blank line inside it.
bool aloneOnItsLine(std::string_view source, size_t at, std::string_view delimiter) {
  const std::string_view line = lineAt(source, at);
  size_t start = 0;
  while (start < line.size() && (line[start] == ' ' || line[start] == '\t')) {
    start++;
  }
  if (start > 3) {
    return false;
  }
  if (line.compare(start, delimiter.size(), delimiter) != 0) {
    return false;
  }
  for (size_t i = start + delimiter.size(); i < line.size(); i++) {
    if (line[i] != ' ' && line[i] != '\t' && line[i] != '\r') {
      return false;
    }
  }
  return true;
}

} // namespace

MathSource rewriteMathDelimiters(std::string_view source, bool brackets) {
  MathSource out;
  out.original = source;
  if (!brackets || source.size() < 4) {
    return out;
  }

  // Most snapshots contain prose or code, not bracket math. Do not build code
  // regions unless a two-byte opener could actually be rewritten.
  size_t candidate = source.find('\\');
  while (candidate != std::string_view::npos &&
         (candidate + 1 == source.size() || (source[candidate + 1] != '(' && source[candidate + 1] != '['))) {
    candidate = source.find('\\', candidate + 1);
  }
  if (candidate == std::string_view::npos) {
    return out;
  }

  // Fenced code regions, visited in source order. Inline spans are skipped by
  // advancing the delimiter search directly to their closing backticks.
  std::vector<std::pair<size_t, size_t>> code;

  // Fences first, so a bracket inside a fenced block is skipped whether the
  // block opens before or after the search reaches it. A `$$` fence is not
  // recognized here: the flag that turns one into a fence also turns these
  // delimiters into `$$`, and the rewritten result is a `$$` fence either way.
  size_t cursor = 0;
  while ((cursor = source.find_first_of("`~", cursor)) != std::string_view::npos) {
    const std::string_view line = lineAt(source, cursor);
    const size_t start = afterIndent(line);
    if (start == kNoOffset) {
      // Every marker on this line would give the same rejected opener.
      const size_t newline = source.find('\n', cursor);
      cursor = newline == std::string_view::npos ? source.size() : newline + 1;
      continue;
    }
    if (start < line.size() && (line[start] == '`' || line[start] == '~')) {
      if (const auto fence = fenceOpener(line)) {
        const size_t lineStart = cursor - start;
        const size_t contentStart = source.find('\n', cursor);
        const size_t begin = contentStart == std::string_view::npos ? source.size() : contentStart + 1;
        // The closing fence, or the end of the source: an unclosed fence runs
        // to the end of its container, and running to the end of the document
        // is the same answer for the only question this scan asks.
        size_t end = begin;
        while (end < source.size()) {
          const size_t lineBegin = end;
          const size_t next = source.find('\n', end);
          const size_t lineEnd = next == std::string_view::npos ? source.size() : next;
          if (closesFence(source.substr(lineBegin, lineEnd - lineBegin), fence->marker, fence->length)) {
            end = next == std::string_view::npos ? source.size() : next + 1;
            break;
          }
          end = lineEnd + 1;
        }
        code.emplace_back(lineStart, end);
        cursor = end;
        continue;
      }
    }
    const size_t newline = source.find('\n', cursor);
    cursor = newline == std::string_view::npos ? source.size() : newline + 1;
  }

  size_t search = 0;
  size_t codeIndex = 0;
  while (search + 1 < source.size()) {
    if (source[search] == '`') {
      if (const auto end = codeSpanEnd(source, search)) {
        search = *end;
        continue;
      }
      // An unclosed run is not a span. The parser will report the rest as text,
      // and a `$` inside it is the characters the writer typed.
      search += backtickRun(source, search);
      continue;
    }
    if (source[search] != '\\') {
      search++;
      continue;
    }
    if (isEscaped(source, search) || search + 1 >= source.size()) {
      // `\\(` is an escaped backslash followed by a literal parenthesis, which
      // is not math, and is exactly what the pair below would otherwise match.
      search += 2;
      continue;
    }
    const char closer = source[search + 1];
    if (closer != '(' && closer != '[') {
      search += 2;
      continue;
    }
    // Inside code, a bracket is two characters and not a delimiter.
    // Fenced regions and this scan both advance in source order. Completed
    // inline code spans already jumped to their end above, so they never need
    // to be stored or revisited for each later formula.
    while (codeIndex < code.size() && code[codeIndex].second <= search) {
      codeIndex++;
    }
    const bool inCode = codeIndex < code.size() && search >= code[codeIndex].first;
    if (inCode) {
      search += 2;
      continue;
    }

    const std::string_view open = source.substr(search, 2);
    const std::string_view close = closer == '(' ? ")" : "]";
    const size_t found = source.find(close, search + 2);
    if (found == std::string_view::npos) {
      // Unpaired: leave it as the CommonMark escape it is, so a reader who meant
      // a literal parenthesis gets one.
      search += 2;
      continue;
    }
    if (found > 0 && isEscaped(source, found - 1)) {
      // The closer's own backslash is part of the delimiter, so the question is
      // whether *that* backslash is escaped: `\)` is an escaped backslash
      // followed by a literal parenthesis, which is not math, while `\)` is the
      // closer. Testing the parenthesis itself would refuse every bracket pair,
      // because the delimiter's backslash always precedes it.
      search += 2;
      continue;
    }

    out.pairs.push_back({search, found + close.size(), closer == '[', aloneOnItsLine(source, search, open)});
    search = found + close.size();
  }

  if (out.pairs.empty()) {
    return out;
  }

  // A same-length copy, so every offset md4c reports is also an offset into the
  // original. That invariant is the only reason the builder can read the
  // original bytes and see what the writer wrote.
  out.text.assign(source);
  for (const MathPair &pair : out.pairs) {
    out.text[pair.open] = '$';
    out.text[pair.open + 1] = '$';
    out.text[pair.close - 2] = '$';
    out.text[pair.close - 1] = '$';
  }
  return out;
}

} // namespace downstream::scan
