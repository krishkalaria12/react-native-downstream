// Host test driver: applies the provisional-styling pass to Markdown from
// stdin and prints the resulting render model as JSON.
//
// The feature bitmask arrives as the first argument, so one binary serves
// every policy a test wants. `__tests__/real-engine-module.ts` drives this so
// the TypeScript suites assert on the real repaired document rather than on a
// mock's idea of one.

#include "Preview.h"
#include "tests/dump_model.h"

#include <cstdlib>
#include <iostream>
#include <iterator>
#include <string>

using namespace downstream;

int main(int argc, char **argv) {
  const std::string source((std::istreambuf_iterator<char>(std::cin)), std::istreambuf_iterator<char>());

  remend::PreviewOptions options;
  if (argc > 1) {
    const long long requested = std::strtoll(argv[1], nullptr, 10);
    if (requested < 0) {
      testdump::writeError("invalid_input");
      return 0;
    }
    options.features =
        static_cast<remend::PreviewOptions::features_t>(requested) & remend::PreviewOptions::allFeatures();
  }

  // A real session only ever previews the tail, and a session's tail here is
  // the whole document: nothing has committed, so there is no prefix to
  // protect. That is the same path `IncrementalParser` takes.
  const remend::Preview preview{options};
  testdump::write(preview.apply(source, 0).parsed);
  return 0;
}
