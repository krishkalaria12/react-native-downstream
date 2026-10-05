// Host test driver: replays one native session through the real
// IncrementalParser and prints the document its last pass published, as the
// JSON `dump_document` prints.
//
// `tests/real-engine-module.ts` drives this, so a TypeScript suite asserts
// on what the session's parser actually does in the mode the stream asked for,
// finish and preview policy included, rather than on the test double's idea of
// those rules.
//
// argv[1] is the mode, `full` or `incremental`, as the TurboModule receives it.
// argv[2] is the parse option bitmask, as the TurboModule's `create` receives it.
// Absent means math is off, so the existing corpus is byte-identical.
// argv[3], when present, is the registered languages, comma separated: the
// model then carries `fenceSlots`, read from the published document exactly as
// the TurboModule's `sessionFenceSlots` reads a snapshot, live until the last
// pass was a finished one. That is what lets a TypeScript suite see a fence the
// incremental parser has not settled, which no fresh parse of the source shows.
// stdin is a sequence of frames: a one-byte operation, a little-endian u32
// payload length, and the payload.
//
//   'a'  append the payload
//   'r'  replace the source with the payload, as a reset does
//   'p'  set the preview policy; the payload is the feature mask as a u32
//   'u'  run a pass on an unfinished stream
//   'f'  run a pass on a finished stream

#include "IncrementalParser.h"
#include "tests/dump_model.h"

#include <cstdint>
#include <cstdio>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>

using namespace downstream;

namespace {

uint32_t readU32(std::string_view bytes) {
  uint32_t value = 0;
  for (size_t i = 0; i < 4; i++) {
    value |= static_cast<uint32_t>(static_cast<unsigned char>(bytes[i])) << (8 * i);
  }
  return value;
}

} // namespace

int main(int argc, char **argv) {
  const std::string mode = argc > 1 ? argv[1] : "full";
  IncrementalParser parser{mode == "incremental" ? ParseMode::IncrementalTail : ParseMode::FullParse,
                           testdump::parseFlagsFromArg(argc, argv, 2)};

  const std::string input((std::istreambuf_iterator<char>(std::cin)), std::istreambuf_iterator<char>());
  const std::string_view frames = input;
  ParseResult last;
  bool parsed = false;
  bool finished = false;
  size_t at = 0;
  while (at < frames.size()) {
    if (frames.size() - at < 5) {
      std::fputs("truncated frame header\n", stderr);
      return 1;
    }
    const char op = frames[at];
    const uint32_t length = readU32(frames.substr(at + 1, 4));
    at += 5;
    if (frames.size() - at < length) {
      std::fputs("truncated frame payload\n", stderr);
      return 1;
    }
    const std::string_view payload = frames.substr(at, length);
    at += length;

    switch (op) {
      case 'a':
        parser.append(payload);
        break;
      case 'r':
        parser.replace(payload);
        break;
      case 'p': {
        if (payload.size() != 4) {
          std::fputs("preview frame needs a u32 mask\n", stderr);
          return 1;
        }
        // The TurboModule masks to the bits the pass defines, and so does this.
        remend::PreviewOptions options;
        options.features = readU32(payload) & remend::PreviewOptions::allFeatures();
        parser.setPreview(options);
        break;
      }
      case 'u':
      case 'f':
        last = parser.parse(op == 'f');
        parsed = true;
        finished = op == 'f';
        break;
      default:
        std::fprintf(stderr, "unknown operation '%c'\n", op);
        return 1;
    }
  }

  if (!parsed) {
    std::fputs("no pass to report\n", stderr);
    return 1;
  }
  // A pass with nothing new returns the published document without a status,
  // so the document the parser holds is the one to print.
  if (last.status == ParseStatus::Ok) {
    last.document = parser.document();
  }
  // The highlighting counters ride along so a test can see that a growing code
  // block resumes from its last complete line instead of being colored again.
  if (!testdump::write(last)) {
    return 0;
  }
  const IncrementalCounters &counters = parser.counters();
  std::printf(",\"highlight\":{\"blocks\":%llu,\"resumes\":%llu,\"complete\":%llu,\"skipped\":%llu,\"budget\":%llu}",
              static_cast<unsigned long long>(counters.blocksHighlighted),
              static_cast<unsigned long long>(counters.highlightResumes),
              static_cast<unsigned long long>(counters.highlightCount(HighlightStop::Complete)),
              static_cast<unsigned long long>(counters.highlightCount(HighlightStop::Skipped)),
              static_cast<unsigned long long>(counters.highlightCount(HighlightStop::Budget)));
  if (argc > 3 && parser.document()) {
    std::printf(",\"fenceSlots\":");
    testdump::writeFenceSlots(fenceSlots(*parser.document(), testdump::languagesFromArg(argc, argv, 3), !finished));
  }
  testdump::writeModelClosed();
  return 0;
}
