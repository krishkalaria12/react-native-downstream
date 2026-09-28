// Host test driver: parses Markdown from stdin and prints the render model
// as JSON for cpp/tests/test_engine.py.

#include "MarkdownParser.h"
#include "Unicode.h"

#include <cstdio>
#include <iostream>
#include <iterator>
#include <string>

using namespace downstream;

namespace {

void writeString(std::string_view text) {
  std::putchar('"');
  for (const char c : text) {
    const auto byte = static_cast<unsigned char>(c);
    switch (c) {
      case '"':
        std::fputs("\\\"", stdout);
        break;
      case '\\':
        std::fputs("\\\\", stdout);
        break;
      case '\n':
        std::fputs("\\n", stdout);
        break;
      case '\t':
        std::fputs("\\t", stdout);
        break;
      default:
        if (byte < 0x20) {
          std::printf("\\u%04x", byte);
        } else {
          std::putchar(c);
        }
    }
  }
  std::putchar('"');
}

void writeString(const std::u16string &text) {
  writeString(utf16ToUtf8(text));
}

} // namespace

int main() {
  const std::string source((std::istreambuf_iterator<char>(std::cin)), std::istreambuf_iterator<char>());
  const ParseResult result = parseMarkdown(source);
  if (result.status != ParseStatus::Ok) {
    std::printf("{\"error\":");
    writeString(std::string(errorCode(result.status)));
    std::printf("}\n");
    return 0;
  }

  const Document &document = *result.document;
  std::printf("{\"blocks\":[");
  for (size_t b = 0; b < document.blocks.size(); b++) {
    const Block &block = document.blocks[b];
    std::printf("%s{\"kind\":%d,\"headingLevel\":%d,\"tableHeader\":%s,\"table\":%u,\"spacing\":%d,\"language\":",
                b ? "," : "", static_cast<int>(block.kind), block.headingLevel, block.tableHeader ? "true" : "false",
                block.table, static_cast<int>(block.spacingBefore));
    writeString(block.language);
    std::printf(",\"text\":");
    writeString(block.text);
    std::printf(",\"containers\":[");
    for (size_t c = 0; c < block.containers.size(); c++) {
      const Container &container = block.containers[c];
      std::printf("%s{\"kind\":%d,\"id\":%u,\"showsMarker\":%s,\"task\":%d,\"marker\":", c ? "," : "",
                  static_cast<int>(container.kind), container.id, container.showsMarker ? "true" : "false",
                  static_cast<int>(container.task));
      writeString(container.marker);
      std::printf("}");
    }
    std::printf("],\"runs\":[");
    for (size_t r = 0; r < block.runs.size(); r++) {
      const InlineRun &run = block.runs[r];
      std::printf("%s[%u,%u,%u,%d]", r ? "," : "", run.start, run.end, run.styles, run.link);
    }
    std::printf("],\"cells\":[");
    for (size_t c = 0; c < block.cells.size(); c++) {
      const TableCell &cell = block.cells[c];
      std::printf("%s[%u,%u,%d]", c ? "," : "", cell.start, cell.end, static_cast<int>(cell.align));
    }
    std::printf("]}");
  }
  std::printf("],\"links\":[");
  for (size_t l = 0; l < document.links.size(); l++) {
    const Link &link = document.links[l];
    std::printf("%s{\"url\":", l ? "," : "");
    writeString(link.url);
    std::printf(",\"range\":[%u,%u]}", link.range.start, link.range.end);
  }
  std::printf("]}\n");
  return 0;
}
