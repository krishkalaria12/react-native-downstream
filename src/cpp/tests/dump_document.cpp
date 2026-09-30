// Host test driver: parses Markdown from stdin and prints the render model
// as JSON for src/cpp/tests/engine.test.mjs.

#include "CodeBlocks.h"
#include "CodeHighlighter.h"
#include "MarkdownParser.h"
#include "tests/dump_model.h"

#include <iostream>
#include <iterator>
#include <string>

using namespace downstream;

int main() {
  const std::string source((std::istreambuf_iterator<char>(std::cin)), std::istreambuf_iterator<char>());
  ParseResult result = parseMarkdown(source);
  // The controlled path colors after the parse and before the platform text
  // work, so the driver colors here too and its JSON is what a view receives.
  if (result.status == ParseStatus::Ok) {
    result.document = highlightSettledCodeBlocks(lexillaHighlighter(), result.document, kHighlightBudgetMs, nullptr);
  }
  if (testdump::write(result)) {
    testdump::writeModelClosed();
  }
  return 0;
}
