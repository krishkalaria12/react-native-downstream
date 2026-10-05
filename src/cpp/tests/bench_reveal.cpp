// Snapshot application only; documents are built before timing. No parser,
// shaping, native draw pass, or frame scheduling is included.
#include "downstream/Reveal.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace downstream;

namespace {
volatile uint64_t consumed = 0;

DocumentHandle snapshot(std::shared_ptr<const SharedList<Block>::Prefix> prefix, std::u16string text) {
  auto document = std::make_shared<Document>();
  Block tail;
  tail.text = std::move(text);
  document->blocks = {std::move(prefix), {std::move(tail)}};
  return document;
}

void workload(const char *name, size_t prefixCount, size_t initialLength, size_t updates,
              size_t passes, RevealSplit split, bool settle) {
  std::vector<Block> committed(prefixCount);
  for (size_t i = 0; i < prefixCount; ++i) {
    committed[i].identity = i + 1;
    committed[i].text = u"a completed paragraph with old visible text";
  }
  auto prefix = SharedList<Block>::Prefix::extend(nullptr, std::move(committed));
  std::u16string text;
  while (text.size() < initialLength) text += u"word ";
  std::vector<DocumentHandle> snapshots{snapshot(prefix, text)};
  for (size_t i = 0; i < updates; ++i) {
    text += u"more ";
    snapshots.push_back(snapshot(prefix, text));
  }
  const DocumentHandle seed = snapshot(prefix, u"");
  RevealOptions options;
  options.effect = RevealEffect::Fade;
  options.split = split;
  options.staggerMs = 0;
  // Compare every unit and its timing separately from the timed loop. The
  // benchmark's consumption below stays constant per snapshot, so measuring a
  // larger active set does not add a checksum walk to engine costs.
  uint64_t semanticChecksum = 1469598103934665603ULL;
  RevealTracker check;
  check.seed(seed);
  double checkNow = 1000;
  const auto hash = [&](uint64_t value) { semanticChecksum = (semanticChecksum ^ value) * 1099511628211ULL; };
  for (const auto &document : snapshots) {
    const auto &units = check.apply(document, checkNow, options);
    hash(units.size());
    for (const auto &unit : units) {
      hash(unit.block); hash(unit.start); hash(unit.end); hash(static_cast<uint64_t>(unit.startAt));
    }
    if (settle) check.prune(checkNow + 1000, options);
    checkNow += settle ? 2000 : 1;
  }
  uint64_t checksum = 0;
  const auto begin = std::chrono::steady_clock::now();
  for (size_t pass = 0; pass < passes; ++pass) {
    RevealTracker tracker;
    // Seed all completed blocks; only the active tail is animated.
    tracker.seed(seed);
    double now = 1000;
    for (const auto &document : snapshots) {
      const auto &units = tracker.apply(document, now, options);
      checksum += units.size();
      if (!units.empty()) checksum += units.back().end;
      if (settle) tracker.prune(now + 1000, options);
      now += settle ? 2000 : 1;
    }
    if (tracker.reReveals() != 0) std::abort();
  }
  consumed = checksum;
  const double milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
  std::printf("{\"name\":\"%s\",\"prefixBlocks\":%zu,\"initialUtf16\":%zu,\"snapshots\":%zu,\"passes\":%zu,\"milliseconds\":%.6f,\"checksum\":%llu,\"semanticChecksum\":\"%llu\"}",
              name, prefixCount, initialLength, snapshots.size(), passes, milliseconds,
              static_cast<unsigned long long>(checksum), static_cast<unsigned long long>(semanticChecksum));
}
} // namespace

int main() {
  std::printf("[\n");
  workload("short_tail_2048_committed_blocks", 2048, 20, 128, 20, RevealSplit::Word, true);
  std::printf(",\n");
  workload("48k_paragraph_appends", 0, 48000, 64, 8, RevealSplit::Word, true);
  std::printf(",\n");
  workload("3000_word_burst_then_appends", 0, 15000, 64, 4, RevealSplit::Word, false);
  std::printf(",\n");
  workload("8000_character_burst_then_appends", 0, 10000, 32, 3, RevealSplit::Char, false);
  std::printf("\n]\n");
}
