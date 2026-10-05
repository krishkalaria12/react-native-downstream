// Engine checks for the controlled fence-slot query the TurboModule answers
// `fenceSlots` with: a fresh answer and a cached one are the same answer, the
// cache keys on everything that can change it, and the synchronous bound says
// `pending` rather than parsing a large source on the calling thread.
//
//     node --test src/cpp/tests/engine.test.mjs   (runs test_controlled_slots)

#include "ControlledSlots.h"
#include "MarkdownParser.h"
#include "PreparedDocument.h"
#include "tests/check.h"

#include <string>
#include <vector>

using namespace downstream;

namespace {

const std::string kChart = "Intro\n\n```chart title=\"Revenue\"\n[1,2]\n```\n\n```json\n{}\n```\n";

bool sameSlots(const std::vector<FenceSlot> &a, const std::vector<FenceSlot> &b) {
  if (a.size() != b.size()) {
    return false;
  }
  for (size_t index = 0; index < a.size(); index++) {
    if (a[index].ordinal != b[index].ordinal || a[index].blockIndex != b[index].blockIndex ||
        a[index].identity != b[index].identity || a[index].language != b[index].language ||
        a[index].meta != b[index].meta || a[index].code != b[index].code ||
        a[index].incomplete != b[index].incomplete) {
      return false;
    }
  }
  return true;
}

void firstQueryAnswers() {
  clearControlledSlotCache();
  // The first query for a source is the one a view mounts with: an empty answer
  // here draws code cards that nothing ever replaces, because the view does not
  // ask again for a source it has an answer for.
  const ControlledSlots first = controlledFenceSlots(kChart, ParseOptions(), {"chart"});
  CHECK(!first.pending, "a small source answers now");
  CHECK(first.slots.size() == 1, "the first query finds the chart fence");
  if (first.slots.size() == 1) {
    CHECK(first.slots[0].ordinal == 0, "the chart is the first code block");
    CHECK_EQ_STR(first.slots[0].language.c_str(), "chart", "language as written");
    CHECK_EQ_STR(first.slots[0].meta.c_str(), "title=\"Revenue\"", "meta after the language");
    CHECK(!first.slots[0].incomplete, "a closed fence is complete");
  }
  const ControlledSlots second = controlledFenceSlots(kChart, ParseOptions(), {"chart"});
  CHECK(sameSlots(first.slots, second.slots), "a cached answer is the fresh answer");
  CHECK(sameSlots(parsedFenceSlots(kChart, ParseOptions(), {"chart"}), first.slots),
        "the worker's parse gives the synchronous answer");
}

void cacheKeysOnLanguages() {
  clearControlledSlotCache();
  CHECK(controlledFenceSlots(kChart, ParseOptions(), {}).slots.empty(), "no languages, no slots");
  const ControlledSlots json = controlledFenceSlots(kChart, ParseOptions(), {"JSON"});
  CHECK(json.slots.size() == 1 && json.slots[0].ordinal == 1,
        "a new language list is a new question, normalized the fence's way");
}

void largeSourceIsPending() {
  clearControlledSlotCache();
  PreparedDocument::clearCache();
  std::string large = kChart;
  while (large.size() <= kSyncSlotBytes) {
    large += "Padding paragraph that makes the source too large to parse here.\n\n";
  }
  const ControlledSlots answer = controlledFenceSlots(large, ParseOptions(), {"chart"});
  CHECK(answer.pending, "past the bound, the caller asks the worker");
  CHECK(answer.slots.empty(), "and gets no slots here");
  CHECK(parsedFenceSlots(large, ParseOptions(), {"chart"}).size() == 1, "the worker finds the fence");
  CHECK(controlledFenceSlots(large, ParseOptions(), {"chart"}).pending, "an unresolved repeat is still pending");
  const auto worker = resolvedFenceSlots(large, ParseOptions(), {"chart"});
  const auto resolved = controlledFenceSlots(large, ParseOptions(), {"chart"});
  CHECK(!resolved.pending && sameSlots(worker, resolved.slots), "a worker answer resolves later sync queries");
  CHECK(sameSlots(worker, resolvedFenceSlots(large, ParseOptions(), {"chart"})),
        "repeated worker queries reuse the resolved answer");
  CHECK(controlledFenceSlots(large, ParseOptions{0}, {"chart"}).pending, "parse flags are part of the cache key");
  CHECK(controlledFenceSlots(large, ParseOptions(), {"json"}).pending, "worker cache keys include languages");
}

void completedPreparationResolvesPending() {
  clearControlledSlotCache();
  PreparedDocument::clearCache();
  const std::string source = kChart + std::string(kSyncSlotBytes, 'x');
  CHECK(controlledFenceSlots(source, ParseOptions(), {"chart"}).pending, "the first large query is pending");
  const auto prepared = PreparedDocument::request(source);
  prepared->runOrWait();
  const auto answer = controlledFenceSlots(source, ParseOptions(), {"chart"});
  CHECK(!answer.pending && answer.slots.size() == 1, "completed preparation replaces an earlier pending miss");

  clearControlledSlotCache();
  const auto worker = resolvedFenceSlots(source, ParseOptions(), {"chart"});
  CHECK(sameSlots(worker, answer.slots), "the worker reads the completed preparation's slots");
  PreparedDocument::clearCache();
  CHECK(!controlledFenceSlots(source, ParseOptions(), {"chart"}).pending,
        "the worker stores a prepared answer for later queries");
}

void emptyAnswersResolve() {
  clearControlledSlotCache();
  PreparedDocument::clearCache();
  const std::string source(kSyncSlotBytes + 1, 'x');
  CHECK(!controlledFenceSlots(source, ParseOptions(), {}).pending, "no registered languages needs no parse");
  CHECK(controlledFenceSlots(source, ParseOptions(), {"chart"}).pending, "an unknown large answer is pending");
  const auto prepared = PreparedDocument::request(source);
  prepared->runOrWait();
  const auto answer = controlledFenceSlots(source, ParseOptions(), {"chart"});
  CHECK(!answer.pending && answer.slots.empty(), "a completed no-match preparation is resolved");

  clearControlledSlotCache();
  PreparedDocument::clearCache();
  CHECK(resolvedFenceSlots(source, ParseOptions(), {"chart"}).empty(), "worker no-match answer is empty");
  CHECK(!controlledFenceSlots(source, ParseOptions(), {"chart"}).pending, "worker empty answer is cached as resolved");

  const std::string invalid(kSyncSlotBytes + 1, '\xff');
  CHECK(resolvedFenceSlots(invalid, ParseOptions(), {"chart"}).empty(), "unreadable source keeps the code-card fallback");
  CHECK(!controlledFenceSlots(invalid, ParseOptions(), {"chart"}).pending, "unreadable worker answer is resolved");
}

void cacheHasCountAndByteBounds() {
  clearControlledSlotCache();
  PreparedDocument::clearCache();
  const std::string first = "```chart\n" + std::string(800 * 1024, 'x') + "\n```\n";
  const std::string second = "```chart\n" + std::string(800 * 1024, 'y') + "\n```\n";
  CHECK(resolvedFenceSlots(first, ParseOptions(), {"chart"}).size() == 1, "first large slot resolves");
  CHECK(!controlledFenceSlots(first, ParseOptions(), {"chart"}).pending, "one large answer fits the byte bound");
  CHECK(resolvedFenceSlots(second, ParseOptions(), {"chart"}).size() == 1, "second large slot resolves");
  CHECK(controlledFenceSlots(first, ParseOptions(), {"chart"}).pending, "byte bound evicts the older large answer");
  CHECK(!controlledFenceSlots(second, ParseOptions(), {"chart"}).pending, "the newest answer remains cached");

  clearControlledSlotCache();
  const std::string oldest(kSyncSlotBytes + 1, 'a');
  resolvedFenceSlots(oldest, ParseOptions(), {"chart"});
  std::string newest;
  for (int index = 0; index < 16; index++) {
    newest = std::to_string(index) + oldest;
    resolvedFenceSlots(newest, ParseOptions(), {"chart"});
  }
  CHECK(controlledFenceSlots(oldest, ParseOptions(), {"chart"}).pending, "entry count evicts the oldest answer");
  CHECK(!controlledFenceSlots(newest, ParseOptions(), {"chart"}).pending, "entry count keeps the newest answer");

  const std::string oversized(5 * 1024 * 1024, 'x');
  CHECK(resolvedFenceSlots(oversized, ParseOptions(), {"chart"}).empty(), "oversized input returns fallback");
  CHECK(controlledFenceSlots(oversized, ParseOptions(), {"chart"}).pending, "a key above the byte bound is not retained");
}

void openFenceIsIncomplete() {
  clearControlledSlotCache();
  const ControlledSlots answer = controlledFenceSlots("```chart\n[1,", ParseOptions(), {"chart"});
  CHECK(answer.slots.size() == 1 && answer.slots[0].incomplete, "an unclosed last fence is still growing");
  const std::string closed = "```chart\nx\n```\n\nText after the fence.\n";
  const auto parsed = parseMarkdown(closed);
  CHECK(parsed.document && parsed.document->settledBlocks == 0, "a bare parse starts unsettled");
  const auto slots = parsedFenceSlots(closed, ParseOptions(), {"chart"});
  CHECK(slots.size() == 1 && !slots[0].incomplete, "controlled queries override settling without copying blocks");
}

} // namespace

int main() {
  firstQueryAnswers();
  cacheKeysOnLanguages();
  largeSourceIsPending();
  completedPreparationResolvesPending();
  emptyAnswersResolve();
  cacheHasCountAndByteBounds();
  openFenceIsIncomplete();
  return ds_check_result();
}
