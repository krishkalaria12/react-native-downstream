// The render model as JSON, shared by the host test drivers so every one of
// them describes a document the same way. `__tests__/engine.ts` reads this
// shape, and a test that asserts on the text a reader would see can only do
// that if the drivers agree on what a field means.

#pragma once

#include "Document.h"
#include "MarkdownParser.h"
#include "Unicode.h"

#include <cstdio>
#include <string>
#include <string_view>

namespace downstream::testdump {

inline void writeString(std::string_view text) {
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

inline void writeString(const std::u16string &text) {
  writeString(utf16ToUtf8(text));
}

/// Prints `{"error":"<code>"}` for a parse that did not produce a document.
inline void writeError(const char *code) {
  std::printf("{\"error\":");
  writeString(std::string(code));
  std::printf("}\n");
}

/// Prints the document, or the error when the parse failed. Identity and
/// sourceRange are left out: they describe where a block came from, and every
/// parse mints new ones.
inline void write(const ParseResult &result) {
  if (result.status != ParseStatus::Ok || !result.document) {
    writeError(errorCode(result.status));
    return;
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
}

} // namespace downstream::testdump
