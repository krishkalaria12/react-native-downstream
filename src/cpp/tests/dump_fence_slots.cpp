// Host test driver: parses Markdown from stdin and prints the fence slots the
// engine would hand JavaScript, as JSON for tests/engine.ts.
//
// argv[1] is the parse option bitmask, as `parseFlagsFromArg` reads it, and
// argv[2] is `live` (`1` or `0`), the same distinction the TurboModule draws:
// a controlled source is live and a finished stream is not. The languages are
// read from argv[3] onward, comma separated, unnormalized — exactly as
// JavaScript sends them, so this driver exercises the engine's own
// normalization.

#include "FenceSlots.h"
#include "MarkdownParser.h"
#include "tests/dump_model.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

using namespace downstream;

int main(int argc, char **argv) {
  const std::string source((std::istreambuf_iterator<char>(std::cin)), std::istreambuf_iterator<char>());
  const ParseOptions parse = testdump::parseFlagsFromArg(argc, argv, 1);
  const bool live = argc > 2 && std::string_view(argv[2]) == "1";
  const std::vector<std::string> languages = testdump::languagesFromArg(argc, argv, 3);

  ParseResult result = parseMarkdown(source, parse);
  if (result.status != ParseStatus::Ok) {
    testdump::writeError(errorCode(result.status));
    return 0;
  }
  // A controlled source is complete the moment it is parsed, so every block
  // settles. This is the rule `PreparedDocument` applies and the TurboModule's
  // synchronous query applies to its own bare parse.
  const auto count =
      static_cast<uint32_t>(std::min<size_t>(result.document->blocks.size(), std::numeric_limits<uint32_t>::max()));
  const DocumentHandle settled = withSettledBlocks(result.document, count);

  const std::vector<FenceSlot> slots = fenceSlots(*settled, languages, live);
  std::printf("{\"slots\":");
  testdump::writeFenceSlots(slots);
  std::printf("}\n");
  return 0;
}
