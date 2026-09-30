// Host test driver: parses Markdown from stdin and prints the render model
// as JSON for src/cpp/tests/engine.test.mjs.

#include "CodeBlocks.h"
#include "CodeHighlighter.h"
#include "MarkdownParser.h"
#include "tests/dump_model.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>

using namespace downstream;

int main() {
  const std::string source((std::istreambuf_iterator<char>(std::cin)), std::istreambuf_iterator<char>());
  ParseResult result = parseMarkdown(source);
  // The controlled path colors after the parse and then marks every block
  // settled, both before the platform text work, so the driver does the same
  // and its JSON is what a view receives.
  if (result.status == ParseStatus::Ok) {
    result.document = highlightSettledCodeBlocks(lexillaHighlighter(), result.document, kHighlightBudgetMs, nullptr);
    const auto count =
        static_cast<uint32_t>(std::min<size_t>(result.document->blocks.size(), std::numeric_limits<uint32_t>::max()));
    result.document = withSettledBlocks(result.document, count);
  }
  if (testdump::write(result)) {
    testdump::writeModelClosed();
  }
  return 0;
}
