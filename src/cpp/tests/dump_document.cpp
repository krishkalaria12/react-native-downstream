// Host test driver: parses Markdown from stdin and prints the render model
// as JSON for src/cpp/tests/engine.test.mjs.

#include "MarkdownParser.h"
#include "tests/dump_model.h"

#include <iostream>
#include <iterator>
#include <string>

using namespace downstream;

int main() {
  const std::string source((std::istreambuf_iterator<char>(std::cin)), std::istreambuf_iterator<char>());
  testdump::write(parseMarkdown(source));
  return 0;
}
