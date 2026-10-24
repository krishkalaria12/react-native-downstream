// Prints the lexer module's raw records for the language cases in
// lexer_cases/, so engine.test.mjs can compare them with the records the same
// module gave when the cases were recorded.
//
// Reads cases from stdin, each "<alias> <byteLength>\n" followed by that many
// UTF-8 bytes. Prints four lines per case: "W" and the records of the whole
// input lexed in one call, then "L" and the records of the input fed one line
// at a time, the way the highlighter feeds it. The line-fed stream saves its
// state after every line and restores it before the next, with another
// language lexed in between, so a resume that lost any state shows up here.
// "H" prints canonical highlighter runs; "S" prints runs after provisional
// 13-unit UTF-16 appends followed by a canonical call on the complete input.
//
// Records print as "end:id", with ends counted from the start of the input and
// records that add nothing dropped. Runs print as "start:end:tokenClass",
// with numeric classes and no Plain runs. A trap prints "TRAP".

#include "CodeHighlighter.h"
#include "Languages.h"
#include "StreamLexer.h"
#include "Unicode.h"

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

using namespace downstream;

namespace {

uint32_t utf16Length(std::string_view bytes) {
  uint32_t length = 0;
  for (const char byte : bytes) {
    const auto unit = static_cast<unsigned char>(byte);
    if ((unit & 0xc0) != 0x80) {
      length += unit >= 0xf0 ? 2 : 1;
    }
  }
  return length;
}

void print(std::string *out, const std::vector<uint32_t> &records, uint32_t base, uint32_t *last) {
  for (size_t index = 0; index + 1 < records.size(); index += 2) {
    const uint32_t end = base + records[index];
    if (end <= *last) {
      continue;
    }
    *last = end;
    *out += ' ';
    *out += std::to_string(end);
    *out += ':';
    *out += std::to_string(static_cast<int32_t>(records[index + 1]));
  }
}

std::string whole(uint8_t language, std::string_view code) {
  std::vector<uint32_t> records;
  StreamLexer *lexer = StreamLexer::current();
  if (lexer == nullptr || !lexer->start(language, nullptr) || !lexer->lex(code, &records)) {
    StreamLexer::discard();
    return "W TRAP";
  }
  std::string out = "W";
  uint32_t last = 0;
  print(&out, records, 0, &last);
  return out;
}

std::string lineFed(uint8_t language, uint8_t other, std::string_view code) {
  // Constructs left open in another language, to disturb the instance between
  // two lines of this one.
  static constexpr std::string_view kNoise = "/* open comment\n\"unterminated `tpl ${ <div class='x\n";
  std::vector<uint32_t> records;
  std::vector<uint8_t> image;
  std::string out = "L";
  uint32_t last = 0;
  uint32_t base = 0;
  for (size_t at = 0; at < code.size();) {
    const size_t newline = code.find('\n', at);
    const size_t end = newline == std::string_view::npos ? code.size() : newline + 1;
    const std::string_view line = code.substr(at, end - at);
    StreamLexer *lexer = StreamLexer::current();
    if (lexer == nullptr || !lexer->start(language, at == 0 ? nullptr : &image) || !lexer->lex(line, &records) ||
        !lexer->capture(&image)) {
      StreamLexer::discard();
      return "L TRAP";
    }
    print(&out, records, base, &last);
    std::vector<uint32_t> noise;
    if (!lexer->start(other, nullptr) || !lexer->lex(kNoise, &noise)) {
      StreamLexer::discard();
    }
    base += utf16Length(line);
    at = end;
  }
  return out;
}

std::string highlighted(std::string_view alias, std::string_view code, bool streamed) {
  const std::u16string text = utf8ToUtf16(code);
  HighlightState state;
  std::vector<TokenRun> runs;
  CodeHighlighter &highlighter = downstreamHighlighter();
  if (streamed) {
    for (size_t at = 0; at < text.size(); at += 13) {
      highlighter.tokenize(alias, std::u16string_view(text).substr(0, std::min(at + 13, text.size())),
                           &state, &runs, 1e9, false);
    }
  }
  highlighter.tokenize(alias, text, &state, &runs, 1e9, true);
  std::string out = streamed ? "S" : "H";
  for (const TokenRun &run : runs) {
    out += ' ';
    out += std::to_string(run.start);
    out += ':';
    out += std::to_string(run.end);
    out += ':';
    out += std::to_string(static_cast<unsigned>(run.token));
  }
  return out;
}

} // namespace

int main() {
  std::ios::sync_with_stdio(false);
  const uint8_t other = findLanguage("js")->id;
  std::string alias;
  size_t length = 0;
  while (std::cin >> alias >> length) {
    std::cin.get();
    std::string code(length, '\0');
    std::cin.read(code.data(), static_cast<std::streamsize>(length));
    const Language *language = findLanguage(alias);
    if (language == nullptr) {
      std::cout << "W UNKNOWN\nL UNKNOWN\nH UNKNOWN\nS UNKNOWN\n";
      continue;
    }
    std::cout << whole(language->id, code) << '\n' << lineFed(language->id, other, code) << '\n'
              << highlighted(alias, code, false) << '\n' << highlighted(alias, code, true) << '\n';
  }
  return 0;
}
