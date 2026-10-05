// Engine checks for the controlled fence-slot query the TurboModule answers
// `fenceSlots` with: a fresh answer and a cached one are the same answer, the
// cache keys on everything that can change it, and the synchronous bound says
// `pending` rather than parsing a large source on the calling thread.
//
//     node --test src/cpp/tests/engine.test.mjs   (runs test_controlled_slots)

#include "ControlledSlots.h"
#include "MarkdownParser.h"
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
    if (a[index].ordinal != b[index].ordinal || a[index].language != b[index].language ||
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
  std::string large = kChart;
  while (large.size() <= kSyncSlotBytes) {
    large += "Padding paragraph that makes the source too large to parse here.\n\n";
  }
  const ControlledSlots answer = controlledFenceSlots(large, ParseOptions(), {"chart"});
  CHECK(answer.pending, "past the bound, the caller asks the worker");
  CHECK(answer.slots.empty(), "and gets no slots here");
  CHECK(parsedFenceSlots(large, ParseOptions(), {"chart"}).size() == 1, "the worker finds the fence");
  CHECK(controlledFenceSlots(large, ParseOptions(), {"chart"}).pending, "a cached miss is still pending");
}

void openFenceIsIncomplete() {
  clearControlledSlotCache();
  const ControlledSlots answer = controlledFenceSlots("```chart\n[1,", ParseOptions(), {"chart"});
  CHECK(answer.slots.size() == 1 && answer.slots[0].incomplete, "an unclosed last fence is still growing");
}

} // namespace

int main() {
  firstQueryAnswers();
  cacheKeysOnLanguages();
  largeSourceIsPending();
  openFenceIsIncomplete();
  return ds_check_result();
}
