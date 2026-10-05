#include "ControlledSlots.h"

#include "PreparedDocument.h"
#include "Unicode.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <mutex>

namespace downstream {

namespace {

/// One cached answer, with every input that could change it.
struct CachedSlots {
  std::string source;
  uint32_t parseFlags{0};
  std::vector<std::string> languages;
  std::vector<FenceSlot> slots;
};

/// Bounded the way `PreparedDocument`'s own cache is: by count, because one
/// chat keeps a screenful of messages and not the whole history.
struct SlotCache {
  std::mutex mutex;
  std::vector<CachedSlots> entries;
  static constexpr size_t kCapacity = 16;
};

SlotCache &slotCache() {
  static auto *instance = new SlotCache();
  return *instance;
}

} // namespace

/// A bare parse: no highlighting, no math layout, no platform preparation,
/// because none of those can change which fences a document has. A controlled
/// source is complete the moment it is parsed, so every block settles — the
/// same rule `PreparedDocument` applies.
std::vector<FenceSlot> parsedFenceSlots(const std::string &source, ParseOptions parse,
                                        const std::vector<std::string> &languages) {
  const ParseResult result = parseMarkdown(source, parse);
  if (result.status != ParseStatus::Ok || !result.document) {
    return {};
  }
  const auto count =
      static_cast<uint32_t>(std::min<size_t>(result.document->blocks.size(), std::numeric_limits<uint32_t>::max()));
  return fenceSlots(*withSettledBlocks(result.document, count), languages, true);
}

ControlledSlots controlledFenceSlots(const std::string &source, ParseOptions parse,
                                     const std::vector<std::string> &languages) {
  ControlledSlots answer;
  SlotCache &cache = slotCache();
  bool cached = false;
  {
    std::lock_guard<std::mutex> lock(cache.mutex);
    for (const CachedSlots &entry : cache.entries) {
      if (entry.source == source && entry.parseFlags == parse.flags && entry.languages == languages) {
        answer.slots = entry.slots;
        cached = true;
        break;
      }
    }
  }
  if (!cached) {
    if (const auto prepared = PreparedDocument::completed(source, parse)) {
      if (prepared->result().status == ParseStatus::Ok && prepared->result().document) {
        answer.slots = fenceSlots(*prepared->result().document, languages, true);
      }
    } else if (source.size() <= kSyncSlotBytes && isValidUtf8(source)) {
      answer.slots = parsedFenceSlots(source, parse, languages);
    }
    std::lock_guard<std::mutex> lock(cache.mutex);
    if (cache.entries.size() >= SlotCache::kCapacity) {
      cache.entries.erase(cache.entries.begin());
    }
    // A copy: the answer is returned from `answer.slots`, so the cache must not
    // take the vector it is about to hand back.
    cache.entries.push_back(CachedSlots{source, parse.flags, languages, answer.slots});
  }
  // Too large to read here, or unreadable at all: the caller draws code cards
  // and asks again through the worker, which is what a source whose
  // preparation is still running does today.
  answer.pending = answer.slots.empty() && source.size() > kSyncSlotBytes;
  return answer;
}

void clearControlledSlotCache() {
  SlotCache &cache = slotCache();
  std::lock_guard<std::mutex> lock(cache.mutex);
  cache.entries.clear();
}

} // namespace downstream
