// Host test driver: parses Markdown from stdin and prints the fence slots the
// engine would hand JavaScript, as JSON for tests/engine.ts.
//
// argv[1] is the math parse flags, as `mathOptionsFromArg` reads them, and
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

namespace {

std::vector<std::string> languagesFromArg(int argc, char **argv, int index) {
  std::vector<std::string> languages;
  if (index >= argc || argv[index] == nullptr) {
    return languages;
  }
  const std::string joined(argv[index]);
  size_t start = 0;
  while (start <= joined.size()) {
    const size_t comma = joined.find(',', start);
    const std::string one = joined.substr(start, comma == std::string::npos ? comma : comma - start);
    if (!one.empty()) {
      languages.push_back(one);
    }
    if (comma == std::string::npos) {
      break;
    }
    start = comma + 1;
  }
  return languages;
}

} // namespace

int main(int argc, char **argv) {
  const std::string source((std::istreambuf_iterator<char>(std::cin)), std::istreambuf_iterator<char>());
  const MathOptions math = testdump::mathOptionsFromArg(argc, argv, 1);
  const bool live = argc > 2 && std::string_view(argv[2]) == "1";
  const std::vector<std::string> languages = languagesFromArg(argc, argv, 3);

  ParseResult result = parseMarkdown(source, math);
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
  std::printf("{\"slots\":[");
  for (size_t index = 0; index < slots.size(); index++) {
    const FenceSlot &slot = slots[index];
    std::printf("%s{\"ordinal\":%u,\"blockIndex\":%zu,\"language\":", index ? "," : "", slot.ordinal,
                slot.blockIndex);
    testdump::writeString(slot.language);
    std::printf(",\"meta\":");
    testdump::writeString(slot.meta);
    std::printf(",\"code\":");
    testdump::writeString(slot.code);
    std::printf(",\"incomplete\":%s,\"identity\":%llu}", slot.incomplete ? "true" : "false",
                static_cast<unsigned long long>(slot.identity));
  }
  std::printf("]}\n");
  return 0;
}
