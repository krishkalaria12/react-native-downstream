// Host test driver: replays one native session through the real
// IncrementalParser and a real RevealTracker, and prints what the tracker
// scheduled after each pass.
//
// The point is that nothing here decides what is new. The session produces the
// same snapshots the app shows, and the tracker aligns them the same way it does
// on a device, so a property about reveals is a statement about the shipped
// engine rather than about a test's idea of it.
//
// argv[1] is the parser mode, `full` or `incremental`.
// stdin is a sequence of frames: a one-byte operation, a little-endian u32
// payload length, and the payload.
//
//   'a'  append the payload
//   'r'  replace the source with the payload, as a reset does
//   'S'  seed the tracker with the payload, as a view binding to a live stream
//        does from the session's own record of what it already showed
//   'u'  run a pass on an unfinished stream
//   'f'  run a pass on a finished stream
//
// Output is one JSON object of passes. Each pass reports the snapshot's block
// texts, whether the stream was still open, and the units the tracker scheduled.
// A unit is a contiguous range, so those are everything a caller needs to check
// that nothing was scheduled twice without agreeing with the tracker about when
// two offsets in two snapshots are the same character.

#include "IncrementalParser.h"
#include "MarkdownParser.h"
#include "Reveal.h"
#include "Unicode.h"

#include <cstdint>
#include <cstdio>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

using namespace downstream;

namespace {

uint32_t readU32(std::string_view bytes) {
  uint32_t value = 0;
  for (size_t i = 0; i < 4; i++) {
    value |= static_cast<uint32_t>(static_cast<unsigned char>(bytes[i])) << (8 * i);
  }
  return value;
}

void writeEscaped(const std::string &text) {
  std::fputc('"', stdout);
  for (const char c : text) {
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
      case '\r':
        std::fputs("\\r", stdout);
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          std::fprintf(stdout, "\\u%04x", static_cast<unsigned char>(c));
        } else {
          std::fputc(c, stdout);
        }
    }
  }
  std::fputc('"', stdout);
}

} // namespace

int main(int argc, char **argv) {
  const std::string mode = argc > 1 ? argv[1] : "full";
  IncrementalParser parser{mode == "incremental" ? ParseMode::IncrementalTail : ParseMode::FullParse};
  RevealTracker tracker;
  RevealOptions options;
  options.effect = RevealEffect::Fade;

  const std::string input((std::istreambuf_iterator<char>(std::cin)), std::istreambuf_iterator<char>());
  const std::string_view frames = input;
  // A clock the caller controls, so the schedule in the output is a function of
  // the input rather than of how fast this process happened to run.
  double now = 0;
  size_t at = 0;
  bool first = true;

  std::fputs("{\"passes\":[", stdout);
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
      case 'S':
        // A view that binds to a stream already in flight. The session's source
        // is what it has, and the tracker is seeded from the same text, so the
        // part the reader already saw does not fade again the way a fresh mount
        // would make it.
        parser.replace(payload);
        tracker.seed(parseMarkdown(payload).document);
        break;
      case 'r':
        parser.replace(payload);
        // A reset clears what has been shown, so the new generation animates
        // from its first word rather than from the old one's tail.
        tracker.clear();
        break;
      case 'u':
      case 'f': {
        parser.parse(op == 'f');
        const DocumentHandle document = parser.document();
        if (!document) {
          break;
        }
        // One snapshot's worth of time per pass, which is what makes the backlog
        // and the stagger observable rather than incidental.
        now += 50;
        const std::vector<RevealUnit> &active = tracker.apply(document, now, options);

        if (!first) {
          std::fputc(',', stdout);
        }
        first = false;
        std::fputs("{\"blocks\":[", stdout);
        for (size_t index = 0; index < document->blocks.size(); index++) {
          if (index > 0) {
            std::fputc(',', stdout);
          }
          writeEscaped(utf16ToUtf8(document->blocks[index].text));
        }
        std::fputs("],\"streaming\":", stdout);
        std::fputs(document->streaming ? "true" : "false", stdout);
        std::fputs(",\"units\":[", stdout);
        for (size_t index = 0; index < active.size(); index++) {
          if (index > 0) {
            std::fputc(',', stdout);
          }
          std::fprintf(stdout, "[%u,%u,%u,%.0f]", active[index].block, active[index].start, active[index].end,
                       active[index].startAt);
        }
        // The characters themselves, one `[block, offset]` per unit rather than
        // per character: a unit is a contiguous range by construction, so a
        // caller reads the same fact from `units` without a document this size.
        std::fputs("],\"rereveals\":", stdout);
        // The invariant the tracker exists to keep, reported by the one thing
        // that knows the alignment. A caller cannot reconstruct it from the
        // units alone, because closing a `**` legitimately reports the word it
        // was styling as new text again.
        std::fprintf(stdout, "%llu}", static_cast<unsigned long long>(tracker.reReveals()));
        break;
      }
      default:
        std::fprintf(stderr, "unknown operation '%c'\n", op);
        return 1;
    }
  }
  std::fputs("]}\n", stdout);
  return 0;
}