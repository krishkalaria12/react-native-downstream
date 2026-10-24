// Host test driver: parses Markdown from stdin, serializes the ranges named on
// the command line, and prints the parse model and every result as one JSON
// object. `src/cpp/tests/engine.test.mjs` and `tests/engine.ts` read it, so
// the export rules are tested against the code that ships rather than against a
// restatement of them.
//
//     dump_export <format> [separator] [configuredBase] [fallback] [operation...]
//
//   format          text | csv | tsv | markdown | html
//   separator       the first UTF-8 unit is the CSV separator, default `,`
//   configuredBase  the `controls.code.download.filename` a caller configured
//   fallback        the base to use when nothing else supplies one
//   operation       `e:<from>:<to>` exports a block range
//                   `d:<index>` prints a code block's download name
//                   `t:<extension>` prints a table's download name
//                   `r:<from>:<to>` exports a table as Markdown, re-parses it
//                   with the real parser, and reports the result as `reparsed`
//
// A download name does not depend on the format, so `d:` and `t:` are reported
// whichever format the run uses.

#include "CodeBlocks.h"
#include "CodeHighlighter.h"
#include "Export.h"
#include "MarkdownParser.h"
#include "tests/dump_model.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>

using namespace downstream;

namespace {

bool formatFor(std::string_view name, ExportFormat &format) {
  if (name == "text") {
    format = ExportFormat::Text;
  } else if (name == "csv") {
    format = ExportFormat::Csv;
  } else if (name == "tsv") {
    format = ExportFormat::Tsv;
  } else if (name == "markdown") {
    format = ExportFormat::Markdown;
  } else if (name == "html") {
    format = ExportFormat::Html;
  } else {
    return false;
  }
  return true;
}

size_t number(std::string_view text) {
  return static_cast<size_t>(std::strtoull(std::string(text).c_str(), nullptr, 10));
}

void range(std::string_view rest, size_t &from, size_t &to) {
  from = rest.empty() ? 0 : number(rest);
  const size_t colon = rest.find(':');
  to = colon == std::string_view::npos ? from + 1 : from + number(rest.substr(colon + 1));
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    std::fputs("usage: dump_export <format> [separator] [base] [fallback] [op...]\n", stderr);
    return 1;
  }
  ExportFormat format = ExportFormat::Text;
  if (!formatFor(argv[1], format)) {
    std::fprintf(stderr, "unknown format '%s'\n", argv[1]);
    return 1;
  }
  const std::string separatorText = argc > 2 ? argv[2] : ",";
  const std::string configured = argc > 3 ? argv[3] : "";
  const std::string fallback = argc > 4 ? argv[4] : "file";
  const char16_t separator = separatorText.empty() ? u',' : static_cast<char16_t>(separatorText[0]);

  const std::string source((std::istreambuf_iterator<char>(std::cin)), std::istreambuf_iterator<char>());
  ParseResult result = parseMarkdown(source);
  if (result.status != ParseStatus::Ok) {
    return 0;
  }
  // The controlled path colors after the parse, so the model an export reads is
  // the one a view holds.
  result.document = highlightSettledCodeBlocks(downstreamHighlighter(), result.document, kHighlightBudgetMs, nullptr);
  const auto settled = static_cast<uint32_t>(
      std::min<size_t>(result.document->blocks.size(), std::numeric_limits<uint32_t>::max()));
  result.document = withSettledBlocks(result.document, settled);

  if (!testdump::write(result)) {
    return 0;
  }
  std::printf(",\"exports\":[");
  bool first = true;
  for (int index = 5; index < argc; index++) {
    const std::string_view operation = argv[index];
    const size_t colon = operation.find(':');
    const std::string_view kind = colon == std::string_view::npos ? operation : operation.substr(0, colon);
    const std::string_view rest = colon == std::string_view::npos ? std::string_view{} : operation.substr(colon + 1);
    std::printf("%s{\"op\":", first ? "" : ",");
    first = false;
    testdump::writeString(std::string(kind));
    if (kind == "e" || kind == "r") {
      size_t from = 0;
      size_t to = 0;
      range(rest, from, to);
      const std::u16string text =
          exportBlocks(*result.document, from, to, kind == "r" ? ExportFormat::Markdown : format, separator);
      std::printf(",\"text\":");
      testdump::writeString(text);
      if (kind == "r") {
        // The round trip runs through the real parser, so a rule that only
        // happens to re-read correctly in this file cannot pass.
        std::printf(",\"reparsed\":");
        testdump::writeDocument(parseMarkdown(utf16ToUtf8(text)));
      }
    } else {
      std::printf(",\"text\":");
      if (kind == "d") {
        const size_t block = number(rest);
        std::string name = fallback;
        if (block < result.document->blocks.size()) {
          const Block &entry = result.document->blocks[block];
          name = downloadName(entry.meta, entry.language, configured, fallback);
        }
        testdump::writeString(name);
      } else if (kind == "t") {
        testdump::writeString(tableDownloadName(configured, rest));
      } else {
        std::fprintf(stderr, "unknown operation '%s'\n", argv[index]);
        return 1;
      }
    }
    std::printf("}");
  }
  std::printf("]");
  testdump::writeModelClosed();
  return 0;
}
