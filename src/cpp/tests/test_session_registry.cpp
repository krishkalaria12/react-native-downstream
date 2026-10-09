#include "SessionRegistry.h"
#include "SessionTrace.h"
#include "check.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

using namespace downstream;

namespace {
std::mutex preparedMutex;
std::vector<LayoutHint> preparedHints;
void captureHints(const DocumentHandle &, const std::vector<LayoutHint> &hints) {
  std::lock_guard<std::mutex> lock(preparedMutex);
  preparedHints = hints;
}
}

int main() {
  std::mutex mutex;
  std::condition_variable ready;
  std::vector<std::tuple<uint64_t, uint64_t>> published;
  const std::string id = "session-test";
  std::mutex traceMutex;
  std::vector<TraceEvent> traces;
  SessionTrace::enable(id, [&](const TraceEvent &event) {
    std::lock_guard<std::mutex> lock(traceMutex);
    traces.push_back(event);
  });
  const auto callback = [&](const SessionEvent &event) {
    if (event.sessionId != id || event.status != ParseStatus::Ok) {
      return;
    }
    {
      std::lock_guard<std::mutex> lock(mutex);
      published.emplace_back(event.generation, event.sequence);
    }
    ready.notify_one();
  };
  const auto await = [&](uint64_t generation, uint64_t sequence) {
    std::unique_lock<std::mutex> lock(mutex);
    return ready.wait_for(lock, std::chrono::seconds(3), [&] {
      for (const auto &event : published) {
        if (event == std::make_tuple(generation, sequence)) {
          return true;
        }
      }
      return false;
    });
  };

  if (!SessionRegistry::create(id, ParseMode::IncrementalTail, callback) ||
      SessionRegistry::create(id, ParseMode::FullParse, callback) ||
      !SessionRegistry::append(id, "hello", 0, 2) || !await(0, 2)) {
    std::cerr << "Initial native session append failed\n";
    return 1;
  }
  auto first = SessionRegistry::snapshot(id, 0, 2);
  if (!first || first->blocks.size() != 1 || first->blocks[0].text != u"hello" ||
      SessionRegistry::snapshot(id, 0, 1)) {
    std::cerr << "Snapshot version mismatch\n";
    return 1;
  }
  const auto firstPublished = SessionRegistry::publishedSnapshot(id, 0, 2);
  CHECK(firstPublished.document == first && !firstPublished.source && firstPublished.live,
        "ordinary Markdown publishes state without copying a source for tag queries");
  CHECK(!SessionRegistry::publishedSnapshot(id, 0, 1).document, "published data requires an exact version");
  SessionTrace::recordDocument(first->id, "measure_start");
  {
    std::lock_guard<std::mutex> lock(traceMutex);
    bool parsed = false;
    bool measured = false;
    for (const auto &event : traces) {
      if (event.sessionId != id || event.generation != 0 || event.sequence != 2 || event.timeNs.empty()) continue;
      parsed |= event.event == "parse_end";
      measured |= event.event == "measure_start";
    }
    if (!parsed || !measured) {
      std::cerr << "Trace lost the snapshot correlation\n";
      return 1;
    }
  }
  for (uint64_t index = 1; index <= 200; index++) {
    auto transient = std::make_shared<Document>();
    transient->id = 1000000 + index;
    SessionTrace::bindDocument(transient, id, 0, index);
  }
  if (SessionTrace::trackedDocumentCount() > 64 ||
      SessionTrace::enabledDocument(1000001)) {
    std::cerr << "Trace retained expired snapshot associations\n";
    return 1;
  }
  {
    auto transient = std::make_shared<Document>();
    transient->id = 2000000;
    SessionTrace::bindDocument(transient, id, 0, 3);
  }
  SessionTrace::recordDocument(2000000, "frame_unavailable");
  {
    std::lock_guard<std::mutex> lock(traceMutex);
    if (traces.empty() || traces.back().event != "frame_unavailable" || traces.back().sequence != 3) {
      std::cerr << "Trace dropped the superseded frame marker\n";
      return 1;
    }
  }

  // A published prefix stays paintable while the next batch is pending.
  if (!SessionRegistry::append(id, " world", 0, 4) || !SessionRegistry::snapshot(id, 0, 2) ||
      !await(0, 4)) {
    std::cerr << "Published prefix was lost\n";
    return 1;
  }
  if (!SessionRegistry::reset(id, "new", 1) || SessionRegistry::snapshot(id, 0, 4) ||
      SessionRegistry::append(id, "stale", 0, 5) || !await(1, 0)) {
    std::cerr << "Reset did not invalidate old work\n";
    return 1;
  }
  CHECK(!SessionRegistry::publishedSnapshot(id, 0, 4).document, "reset invalidates the old published tuple");
  auto replacement = SessionRegistry::snapshot(id, 1, 0);
  if (!replacement || replacement->blocks.size() != 1 || replacement->blocks[0].text != u"new" ||
      SessionRegistry::reset(id, std::string(1, static_cast<char>(0xff)), 2) ||
      !SessionRegistry::snapshot(id, 1, 0) ||
      !SessionRegistry::finish(id, 1, 0) || SessionRegistry::append(id, "late", 1, 1)) {
    std::cerr << "Finished session accepted another append\n";
    return 1;
  }
  SessionRegistry::dispose(id);
  if (SessionRegistry::snapshot(id, 1, 0) || SessionRegistry::append(id, "late", 1, 1) ||
      SessionTrace::enabled(id) || SessionTrace::enabledDocument(first->id)) {
    std::cerr << "Disposed session remained accessible\n";
    return 1;
  }

  // A provisional-styling policy can be set on a running session, takes effect
  // without any new input, and leaves the canonical source alone. Turning it
  // back off restores the literal markers.
  //
  // Waiting is by content rather than by event count: a policy change publishes
  // under the sequence already in flight, and the pass can finish before a
  // waiter has looked. The callback runs under the session lock, so the
  // snapshot is read from here rather than from inside it.
  const auto shows = [](const std::string &session, uint64_t generation, uint64_t sequence,
                        const std::u16string &text) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    for (;;) {
      const auto snapshot = SessionRegistry::snapshot(session, generation, sequence);
      if (snapshot && !snapshot->blocks.empty() && snapshot->blocks.back().text == text) {
        return snapshot;
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        return DocumentHandle{};
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  };
  const auto ignore = [](const SessionEvent &) {};

  // Wait for the worker to consume the original options before changing only
  // tags. A consumed pending update must not erase the configured flags.
  for (const ParseMode mode : {ParseMode::FullParse, ParseMode::IncrementalTail}) {
    const std::string policyId = mode == ParseMode::FullParse ? "policy-full" : "policy-incremental";
    CHECK(SessionRegistry::create(policyId, mode, ignore), "create a session for independent tag updates");
    CHECK(SessionRegistry::setParse(policyId, ParseOptions((ParseFlags::Default | ParseFlags::InlineHtml) &
                                                         ~ParseFlags::Cjk)),
          "configure HTML and disable CJK before the worker runs");
    CHECK(SessionRegistry::append(policyId, "test <mark>ready</mark> これは~~削除~~です\n", 0, 1),
          "append source that distinguishes configured flags from defaults");
    CHECK(shows(policyId, 0, 1, u"test ready これは~~削除~~です"), "wait until the worker has consumed the flags");
    CHECK(SessionRegistry::setParseTags(policyId, tagPolicyFromStrings({"-1", "user", "id"})),
          "change only the tags after the flags have been consumed");
    CHECK(SessionRegistry::append(policyId, "\n<user id=\"7\">\n\nhello\n\n</user>\n", 0, 2),
          "append an allowed custom tag after changing its policy");
    const auto policyDocument = shows(policyId, 0, 2, u"hello");
    CHECK(policyDocument && policyDocument->blocks.front().text == u"test ready これは~~削除~~です",
          "tag-only updates preserve HTML support and the disabled CJK flag");
    CHECK(policyDocument && !policyDocument->tags.empty(), "tag-only updates apply the new tag policy");
    SessionRegistry::dispose(policyId);
  }

  const std::string tagsId = "tag-source-session";
  CHECK(SessionRegistry::create(tagsId, ParseMode::FullParse, ignore), "create a custom-tag session");
  CHECK(SessionRegistry::setParse(tagsId, ParseOptions(ParseFlags::Default | ParseFlags::InlineHtml,
                                                      tagPolicyFromStrings({"-1", "user", "id"}))),
        "allow custom containers with their attribute policy");
  CHECK(SessionRegistry::append(tagsId, "<user id=\"7\">\n\nhello\n", 0, 1), "append an open custom container");
  CHECK(shows(tagsId, 0, 1, u"hello"), "publish the open custom container");
  const auto tagPrefix = SessionRegistry::publishedSnapshot(tagsId, 0, 1);
  CHECK(tagPrefix.document && tagPrefix.source && *tagPrefix.source == "<user id=\"7\">\n\nhello\n" && tagPrefix.live,
        "custom tag publication keeps canonical source and live state together");
  CHECK(SessionRegistry::append(tagsId, "\n</user>\n\nworld\n", 0, 2), "close the container and append a paragraph");
  CHECK(shows(tagsId, 0, 2, u"world"), "publish the extended custom source");
  const auto tagExtended = SessionRegistry::publishedSnapshot(tagsId, 0, 2);
  CHECK(tagExtended.source && *tagExtended.source == "<user id=\"7\">\n\nhello\n\n</user>\n\nworld\n",
        "the new document has the new canonical source");
  CHECK(tagPrefix.source && *tagPrefix.source == "<user id=\"7\">\n\nhello\n",
        "retained published custom source is immutable");
  SessionRegistry::dispose(tagsId);

  const std::string previewId = "preview-session";
  CHECK(SessionRegistry::create(previewId, ParseMode::IncrementalTail, ignore), "create a session to preview on");
  CHECK(SessionRegistry::append(previewId, "hello **bold wor", 0, 1), "append to the preview session");
  CHECK(shows(previewId, 0, 1, u"hello **bold wor"), "an unset policy leaves the markers literal");

  // Unknown features are masked to what the pass defines, so a stale caller
  // cannot ask for something that does not exist and appear to succeed.
  remend::PreviewOptions options;
  options.features = remend::PreviewOptions::allFeatures() | 0x80000000u;
  CHECK(SessionRegistry::setPreview(previewId, options), "set a policy on a running session");
  const auto styled = shows(previewId, 0, 1, u"hello bold wor");
  CHECK(styled, "the policy restyles the tail with no new input");
  CHECK(styled && !styled->blocks.back().runs.empty(), "the restyled tail is styled");
  CHECK(styled && styled->blocks.back().runs.front().styles == InlineStyle::Strong, "the span is strong");

  CHECK(SessionRegistry::setPreview(previewId, remend::PreviewOptions{}), "turn the policy back off");
  CHECK(shows(previewId, 0, 1, u"hello **bold wor"), "turning the policy off restores the literal markers");
  SessionRegistry::dispose(previewId);

  // A finished stream ignores the policy entirely, so the settled document is
  // a parse of the bytes the writer sent. An unclosed span is the case that
  // tells the two apart: previewed it would read as bold, settled it is
  // literal. Full mode is what createMarkdownStream uses, and it reaches the
  // parser by its own branch, so both modes are asked.
  for (const ParseMode mode : {ParseMode::FullParse, ParseMode::IncrementalTail}) {
    const std::string settledId = mode == ParseMode::FullParse ? "settled-full" : "settled-incremental";
    CHECK(SessionRegistry::create(settledId, mode, ignore), "create a session to settle");
    CHECK(SessionRegistry::setPreview(settledId, remend::PreviewOptions::all()),
          "enable the policy before any input");
    CHECK(SessionRegistry::append(settledId, "hello **bold wor", 0, 1), "append an open span");
    CHECK(shows(settledId, 0, 1, u"hello bold wor"), "the open span previews as bold while the stream runs");

    CHECK(SessionRegistry::finish(settledId, 0, 1), "finish with the span still open");
    const auto settled = shows(settledId, 0, 1, u"hello **bold wor");
    CHECK(settled, "a finished stream is never repaired");
    CHECK(settled && settled->blocks.back().runs.empty(), "a finished stream leaves the open span unstyled");
    const auto finishedPublished = SessionRegistry::publishedSnapshot(settledId, 0, 1);
    CHECK(finishedPublished.document == settled && !finishedPublished.live,
          "finished state is published with its settled document");

    // Asking again after the stream settled changes nothing a reader can see.
    CHECK(SessionRegistry::setPreview(settledId, remend::PreviewOptions::all()),
          "a settled session accepts a policy");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(SessionRegistry::snapshot(settledId, 0, 1) == settled, "a settled session is not reparsed");
    SessionRegistry::dispose(settledId);
  }

  CHECK(!SessionRegistry::setPreview("no-such-session", remend::PreviewOptions::all()),
        "an unknown session rejects a policy change");

  // Each subscriber carries its own native token. Settled bodies are exported
  // once; later prose and a second policy must not invalidate that lineage.
  for (const ParseMode mode : {ParseMode::FullParse, ParseMode::IncrementalTail}) {
    const std::string slotsId = mode == ParseMode::FullParse ? "slots-full" : "slots-incremental";
    CHECK(SessionRegistry::create(slotsId, mode, ignore), "create a slot-delta session");
    CHECK(SessionRegistry::append(slotsId, "```mermaid title\ngraph TD; A-->B\n```\n\n```chart\nsecond\n```\n\ntail", 0, 1),
          "append settled fences and a growing tail");
    CHECK(shows(slotsId, 0, 1, u"tail"), "publish initial custom fences");
    CHECK(SessionRegistry::append(slotsId, "\n\nmore", 0, 2), "prove the leading fence identities");
    CHECK(shows(slotsId, 0, 2, u"more"), "publish proven identities");
    const auto firstSlots = SessionRegistry::fenceSlotChanges(slotsId, 0, 2, {"mermaid", "chart"}, 0);
    CHECK(firstSlots.token != 0 && firstSlots.changed.size() == 2 && firstSlots.order.size() == 2,
          "the first subscriber receives both payloads");
    const auto secondSlots = SessionRegistry::fenceSlotChanges(slotsId, 0, 2, {"CHART", "MERMAID", "chart"}, 0);
    CHECK(secondSlots.token == firstSlots.token && secondSlots.changed.size() == 2,
          "another subscriber receives a full answer under the normalized policy");
    const auto subset = SessionRegistry::fenceSlotChanges(slotsId, 0, 2, {"chart"}, firstSlots.token);
    CHECK(subset.changed.size() == 1 && subset.order.size() == 1 && subset.changed[0]->code == u"second",
          "a policy change receives its full payload instead of borrowing another policy token");
    const auto beforeWork = SessionRegistry::slotWork(slotsId);
    CHECK(SessionRegistry::append(slotsId, "\n\nlast", 0, 3), "append unrelated prose");
    CHECK(shows(slotsId, 0, 3, u"last"), "publish unrelated prose");
    const auto nextSlots = SessionRegistry::fenceSlotChanges(slotsId, 0, 3, {"chart", "mermaid"}, firstSlots.token);
    const auto afterWork = SessionRegistry::slotWork(slotsId);
    CHECK(nextSlots.token != firstSlots.token && nextSlots.changed.empty() && nextSlots.order == firstSlots.order,
          "later prose sends order and token without re-exporting settled fence bodies");
    CHECK(afterWork.payloadBuildBytes == beforeWork.payloadBuildBytes &&
          afterWork.payloadExportBytes == beforeWork.payloadExportBytes && afterWork.reusedPayloads >= 2,
          "work counters prove the settled payloads were neither copied nor exported");
    CHECK(SessionRegistry::fenceSlotChanges(slotsId, 0, 3, {"chart", "mermaid"}, secondSlots.token).changed.empty(),
          "both subscribers independently advance from the same old token");
    CHECK(SessionRegistry::reset(slotsId, "```chart\nreplacement\n```", 1), "reset the slot generation");
    CHECK(shows(slotsId, 1, 0, u"replacement"), "publish replacement fence");
    const auto replacementSlots = SessionRegistry::fenceSlotChanges(slotsId, 1, 0, {"chart"}, subset.token);
    CHECK(replacementSlots.changed.size() == 1 && replacementSlots.order.size() == 1 &&
          replacementSlots.changed[0]->code == u"replacement", "reset sends a fresh complete payload and removes old IDs");
    SessionRegistry::dispose(slotsId);
    CHECK(SessionRegistry::fenceSlotChanges(slotsId, 1, 0, {"chart"}, replacementSlots.token).token == 0,
          "disposed sessions discard slot lineage");
  }

  const std::string unfinishedSlots = "unfinished-slots";
  CHECK(SessionRegistry::create(unfinishedSlots, ParseMode::IncrementalTail, ignore), "create unfinished fence session");
  CHECK(SessionRegistry::append(unfinishedSlots, "```mermaid\nopen", 0, 1), "append an unclosed fence");
  CHECK(shows(unfinishedSlots, 0, 1, u"open"), "publish unfinished fence");
  const auto openSlots = SessionRegistry::fenceSlotChanges(unfinishedSlots, 0, 1, {"mermaid"}, 0);
  CHECK(openSlots.changed.size() == 1 && openSlots.changed[0]->incomplete, "open fence is incomplete");
  CHECK(SessionRegistry::finish(unfinishedSlots, 0, 1), "finish without another input sequence");
  for (size_t attempt = 0; attempt < 1500 && SessionRegistry::publishedSnapshot(unfinishedSlots, 0, 1).live; ++attempt)
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  const auto finalSlots = SessionRegistry::fenceSlotChanges(unfinishedSlots, 0, 1, {"mermaid"}, openSlots.token);
  CHECK(finalSlots.token != openSlots.token && finalSlots.changed.size() == 1 && !finalSlots.changed[0]->incomplete,
        "finish publishes a changed payload at the same input sequence");
  SessionRegistry::dispose(unfinishedSlots);

  const std::string tagSlotsId = "tag-slot-deltas";
  CHECK(SessionRegistry::create(tagSlotsId, ParseMode::IncrementalTail, ignore), "create sparse tag session");
  CHECK(SessionRegistry::setParse(tagSlotsId, ParseOptions(ParseFlags::Default | ParseFlags::InlineHtml,
      tagPolicyFromStrings({"-1", "chart", "id"}))), "configure custom tag policy");
  CHECK(SessionRegistry::append(tagSlotsId, "<chart id=\"a\">\n\nlarge body\n\n</chart>\n\ntail", 0, 1), "append closed custom tag");
  CHECK(shows(tagSlotsId, 0, 1, u"tail"), "publish sparse body snapshot");
  const auto sparseFirst = SessionRegistry::publishedSnapshot(tagSlotsId, 0, 1, false);
  CHECK(sparseFirst.document && !sparseFirst.source && sparseFirst.tagBodies && sparseFirst.tagStructure &&
        sparseFirst.tagBodies->size() == 1 && *(*sparseFirst.tagBodies)[0] == "large body",
        "native sparse queries preserve exact bodies without flattening canonical source");
  CHECK(SessionRegistry::append(tagSlotsId, "\n\nmore", 0, 2), "append prose after closed tag");
  CHECK(shows(tagSlotsId, 0, 2, u"more"), "publish next sparse snapshot");
  const auto sparseNext = SessionRegistry::publishedSnapshot(tagSlotsId, 0, 2, false);
  CHECK(sparseNext.tagBodies && (*sparseFirst.tagBodies)[0] == (*sparseNext.tagBodies)[0],
        "closed source slices share exact immutable storage across snapshots");
  const auto tags = SessionRegistry::tagSlotChanges(tagSlotsId, 0, 2, {"chart"}, 0);
  const auto tagWorkBefore = SessionRegistry::slotWork(tagSlotsId);
  CHECK(tags.changed.size() == 1 && tags.changed[0]->markdown == "large body", "sparse source answers tag payloads");
  CHECK(SessionRegistry::append(tagSlotsId, "\n\nlast", 0, 3), "append more unrelated tag prose");
  CHECK(shows(tagSlotsId, 0, 3, u"last"), "publish next tag snapshot");
  const auto laterTags = SessionRegistry::tagSlotChanges(tagSlotsId, 0, 3, {"chart"}, tags.token);
  const auto tagWorkAfter = SessionRegistry::slotWork(tagSlotsId);
  CHECK(laterTags.changed.empty() && laterTags.order == tags.order &&
        tagWorkAfter.payloadBuildBytes == tagWorkBefore.payloadBuildBytes &&
        tagWorkAfter.payloadExportBytes == tagWorkBefore.payloadExportBytes,
        "tag payload work stays constant when only later prose changes");
  CHECK(tagWorkAfter.tagBodyCopyBytes == tagWorkBefore.tagBodyCopyBytes &&
        tagWorkAfter.sourceCaptureBytes - tagWorkBefore.sourceCaptureBytes == 6,
        "source capture copies only appended bytes and reuses the closed tag body");
  const auto builds = SessionRegistry::slotWork(tagSlotsId).structureBuilds;
  SessionRegistry::tagSlotChanges(tagSlotsId, 0, 3, {"other", "chart"}, 0);
  CHECK(SessionRegistry::slotWork(tagSlotsId).structureBuilds == builds,
        "configuration variants share the snapshot structural plan");
  CHECK(SessionRegistry::reset(tagSlotsId, "<chart>\n\nreplacement\n\n</chart>\n", 1), "reset sparse tag source");
  CHECK(shows(tagSlotsId, 1, 0, u"replacement"), "publish replacement custom source");
  const auto resetTags = SessionRegistry::tagSlotChanges(tagSlotsId, 1, 0, {"chart"}, tags.token);
  CHECK(resetTags.changed.size() == 1 && resetTags.changed[0]->markdown == "replacement",
        "replacement lineage cannot reuse a prior canonical body range");
  CHECK(*(*sparseFirst.tagBodies)[0] == "large body", "old sparse snapshots remain immutable after reset");
  SessionRegistry::dispose(tagSlotsId);

  const std::string lateTagsId = "late-tag-closure";
  CHECK(SessionRegistry::create(lateTagsId, ParseMode::IncrementalTail, ignore), "create late tag closure session");
  CHECK(SessionRegistry::setParse(lateTagsId, ParseOptions(ParseFlags::Default | ParseFlags::InlineHtml,
      tagPolicyFromStrings({"-1", "chart"}))), "allow late tags");
  CHECK(SessionRegistry::append(lateTagsId, "<chart>\n\nouter", 0, 1) && shows(lateTagsId, 0, 1, u"outer"), "publish open outer tag");
  const auto outerSlots = SessionRegistry::tagSlotChanges(lateTagsId, 0, 1, {"chart"}, 0);
  CHECK(outerSlots.changed.size() == 1 && outerSlots.changed[0]->incomplete, "initial outer tag is hosted provisionally");
  CHECK(SessionRegistry::append(lateTagsId, "\n\n<chart>\n\ninner\n\n</chart>\n\nafter\n\n</chart>\n", 0, 2) &&
        shows(lateTagsId, 0, 2, u"after"), "publish nested tag and both closures");
  const auto nestedSlots = SessionRegistry::tagSlotChanges(lateTagsId, 0, 2, {"chart"}, outerSlots.token);
  CHECK(nestedSlots.changed.size() == 1 && nestedSlots.order.size() == 1 &&
        nestedSlots.order[0] != outerSlots.order[0] && nestedSlots.changed[0]->markdown == "inner" &&
        !nestedSlots.changed[0]->incomplete, "nested hosting removes the partially hostable outer slot and adds the exact inner body");
  const auto removedPolicy = SessionRegistry::tagSlotChanges(lateTagsId, 0, 2, {"unknown"}, nestedSlots.token);
  CHECK(removedPolicy.order.empty() && removedPolicy.changed.empty(), "renderer policy removals leave no obsolete hosted slot");
  SessionRegistry::dispose(lateTagsId);

  SessionSlotCache<FenceSlot> bounded;
  auto payload = std::make_shared<FenceSlot>();
  payload->ordinal = 7;
  payload->code = u"body";
  const auto buildPayload = [&](const auto &) { return std::vector<std::shared_ptr<const FenceSlot>>{payload}; };
  const auto oldest = bounded.query(1, 0, true, {"chart"}, 0, buildPayload);
  for (uint64_t document = 2; document < 20; ++document) bounded.query(document, 0, true, {"chart"}, 0, buildPayload);
  const auto evicted = bounded.query(20, 0, true, {"chart"}, oldest.token, buildPayload);
  CHECK(evicted.changed.size() == 1 && evicted.order == oldest.order,
        "a lagging subscriber whose token was evicted receives a complete recoverable delta");
  CHECK(bounded.work().retainedBytes <= 8 * 1024 * 1024, "retained metadata and payload bytes obey the cache cap");
  auto large = std::make_shared<FenceSlot>();
  large->code.assign(5 * 1024 * 1024, u'x');
  SessionSlotCache<FenceSlot> oversized;
  const auto buildLarge = [&](const auto &) { return std::vector<std::shared_ptr<const FenceSlot>>{large}; };
  const auto largeFirst = oversized.query(1, 0, true, {"chart"}, 0, buildLarge);
  const auto largeNext = oversized.query(2, 0, true, {"chart"}, largeFirst.token, buildLarge);
  CHECK(oversized.work().retainedBytes == 0 && largeNext.changed.size() == 1,
        "an oversized body is delivered without retaining an unbounded cache entry");

  // Preparation keeps both exact measured/mounted widths while every other
  // geometry input matches, and discards obsolete variants on a policy change.
  RenderPreparer::install(captureHints);
  const std::string widthsId = "exact-width-hints";
  CHECK(SessionRegistry::create(widthsId, ParseMode::IncrementalTail, ignore), "create exact-width session");
  SessionRegistry::noteLayout(widthsId, LayoutHint{1, 320.2f});
  SessionRegistry::noteLayout(widthsId, LayoutHint{1, 320.f});
  CHECK(SessionRegistry::append(widthsId, "width", 0, 1) && shows(widthsId, 0, 1, u"width"), "prepare both exact widths");
  {
    std::lock_guard<std::mutex> lock(preparedMutex);
    CHECK(preparedHints.size() == 2 && preparedHints[0].width == 320.f && preparedHints[1].width == 320.2f,
          "worker preparation preserves mounted and measured widths exactly");
  }
  SessionRegistry::noteLayout(widthsId, LayoutHint{1.5f, 320.f});
  CHECK(SessionRegistry::append(widthsId, " changed", 0, 2) && shows(widthsId, 0, 2, u"width changed"), "prepare new font policy");
  {
    std::lock_guard<std::mutex> lock(preparedMutex);
    CHECK(preparedHints.size() == 1 && preparedHints[0].fontScale == 1.5f,
          "geometry changes evict obsolete alternate widths");
  }
  auto incrementalHint = LayoutHint{1.5f, 280.f};
  incrementalHint.incrementalParagraphs = true;
  SessionRegistry::noteLayout(widthsId, incrementalHint);
  CHECK(SessionRegistry::append(widthsId, " incremental", 0, 3) &&
        shows(widthsId, 0, 3, u"width changed incremental"), "prepare incremental paragraph policy");
  {
    std::lock_guard<std::mutex> lock(preparedMutex);
    CHECK(preparedHints.size() == 1 && preparedHints[0].width == 280.f &&
          preparedHints[0].incrementalParagraphs,
          "paragraph wrapping policy changes evict obsolete canonical widths");
  }
  SessionRegistry::noteLayout(widthsId, LayoutHint{1.5f, 320.f});
  CHECK(SessionRegistry::append(widthsId, " canonical", 0, 4) &&
        shows(widthsId, 0, 4, u"width changed incremental canonical"), "restore canonical paragraph policy");
  {
    std::lock_guard<std::mutex> lock(preparedMutex);
    CHECK(preparedHints.size() == 1 && !preparedHints[0].incrementalParagraphs,
          "canonical policy restoration drops incremental preparation variants");
  }
  SessionRegistry::dispose(widthsId);
  RenderPreparer::install(nullptr);

  // Dispose returns while a large parse is in flight and no event follows it.
  const std::string busy = "busy-session";
  std::atomic<int> busyEvents{0};
  std::atomic<bool> disposed{false};
  std::atomic<bool> lateEvent{false};
  if (!SessionRegistry::create(busy, ParseMode::FullParse, [&](const SessionEvent &) {
        busyEvents++;
        if (disposed) lateEvent = true;
      })) {
    std::cerr << "Could not create busy session\n";
    return 1;
  }
  std::string chunk;
  while (chunk.size() < 60 * 1024) chunk += "- **item** with [a link](https://example.com) and `code`\n";
  for (uint64_t sequence = 1; sequence <= 12; sequence++) {
    if (!SessionRegistry::append(busy, chunk, 0, sequence)) {
      std::cerr << "Busy append failed\n";
      return 1;
    }
  }
  SessionRegistry::dispose(busy);
  disposed = true;
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  if (lateEvent || SessionRegistry::create(busy, ParseMode::FullParse, callback) == false) {
    std::cerr << "Disposed session published an event or kept its ID\n";
    return 1;
  }
  SessionRegistry::dispose(busy);
  // This flow returns early on a failed dispose, because continuing would race
  // against a live worker; the earlier failures return with a message above.
  CHECK(ds_check_failures == 0, "session registry lifecycle");
  return ds_check_result();
}
