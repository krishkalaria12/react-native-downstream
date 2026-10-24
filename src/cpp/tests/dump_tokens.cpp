// Host test driver: tokenizes one fenced code block and prints its runs as
// JSON for src/cpp/tests/engine.test.mjs. Input is `info\n` followed by the
// block text, both UTF-8, so a fixture is one readable string.

#include "CodeHighlighter.h"
#include "Unicode.h"
#include "tests/dump_model.h"

#include <iostream>
#include <iterator>
#include <string>
#include <vector>

using namespace downstream;

namespace {

/// A UTF-16 run as a JSON string, quotes included. Code text can hold a quote
/// or a control character even though a code block rarely does, and the
/// fixtures read as one object per run.
std::string quoted(std::u16string_view text) {
  std::string out = "\"";
  for (size_t index = 0; index < text.size(); index++) {
    const char16_t unit = text[index];
    if (unit == u'"' || unit == u'\\') {
      out += '\\';
      out += static_cast<char>(unit);
    } else if (unit < 0x20) {
      char escape[8];
      snprintf(escape, sizeof(escape), "\\u%04x", static_cast<unsigned>(unit));
      out += escape;
    } else {
      out += utf16ToUtf8(std::u16string_view(&unit, 1));
    }
  }
  out += '"';
  return out;
}

} // namespace

int main() {
  const std::string input((std::istreambuf_iterator<char>(std::cin)), std::istreambuf_iterator<char>());
  const size_t split = input.find('\n');
  const std::u16string info = utf8ToUtf16(split == std::string::npos ? input : input.substr(0, split));
  const std::u16string text =
      utf8ToUtf16(split == std::string::npos ? std::string() : input.substr(split + 1));

  HighlightState state;
  std::vector<TokenRun> runs;
  downstreamHighlighter().tokenize(utf16ToUtf8(info), text, &state, &runs, kHighlightBudgetMs);

  std::string out = "{\"info\":";
  out += quoted(info);
  out += ",\"text\":";
  out += quoted(text);
  out += ",\"runs\":[";
  for (size_t index = 0; index < runs.size(); index++) {
    if (index > 0) {
      out += ',';
    }
    out += '[';
    out += std::to_string(runs[index].start);
    out += ',';
    out += std::to_string(runs[index].end);
    out += ",\"";
    out += testdump::tokenName(runs[index].token);
    out += "\",";
    out += quoted(text.substr(runs[index].start, runs[index].end - runs[index].start));
    out += ']';
  }
  out += "]}";
  out += '\n';
  fwrite(out.data(), 1, out.size(), stdout);
  return 0;
}
