// Host test driver: parses Markdown from stdin and prints the render model
// as JSON for src/cpp/tests/engine.test.mjs.
//
// argv[1] is the parse option bitmask, as `parseFlagsFromArg` reads them.
// Absent means every bit is off, so the existing corpus sees exactly the parse
// it saw before any of these options existed; a case about a formula, a tag, or
// an opt-in syntax names its bits.

#include "CodeBlocks.h"
#include "CodeHighlighter.h"
#include "MarkdownParser.h"
#include "math/MathBlocks.h"
#include "tests/dump_model.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>

using namespace downstream;

int main(int argc, char **argv) {
  const std::string source((std::istreambuf_iterator<char>(std::cin)), std::istreambuf_iterator<char>());
  const ParseOptions parse = testdump::parseFlagsFromArg(argc, argv, 1);
  ParseResult result = parseMarkdown(source, parse);
  // The controlled path colors after the parse, lays out its formulas, and then
  // marks every block settled, all before the platform text work, so the driver
  // does the same and its JSON is what a view receives.
  if (result.status == ParseStatus::Ok) {
    result.document = highlightSettledCodeBlocks(lexillaHighlighter(), result.document, kHighlightBudgetMs, nullptr);
    result.document = layoutMath(result.document);
    const auto count =
        static_cast<uint32_t>(std::min<size_t>(result.document->blocks.size(), std::numeric_limits<uint32_t>::max()));
    result.document = withSettledBlocks(result.document, count);
  }
  if (testdump::write(result)) {
    testdump::writeModelClosed();
  }
  return 0;
}
