#include "ControlledSlots.h"

#include "PreparedDocument.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <list>
#include <mutex>

namespace downstream {

namespace {

struct CachedSlots {
  std::string source;
  uint32_t parseFlags{0};
  std::vector<std::string> languages;
  std::vector<FenceSlot> slots;
  size_t bytes{0};
};

struct SlotCache {
  std::mutex mutex;
  // Most recently queried first. Only resolved answers belong in this cache.
  std::list<CachedSlots> entries;
  size_t bytes{0};
  static constexpr size_t kCapacity = 16;
  static constexpr size_t kByteCapacity = 4 * 1024 * 1024;
};

SlotCache &slotCache() {
  static auto *instance = new SlotCache();
  return *instance;
}

bool sameQuery(const CachedSlots &entry, const std::string &source, ParseOptions parse,
               const std::vector<std::string> &languages) {
  return entry.parseFlags == parse.flags && entry.source == source && entry.languages == languages;
}

bool cachedSlots(const std::string &source, ParseOptions parse, const std::vector<std::string> &languages,
                 std::vector<FenceSlot> &slots) {
  SlotCache &cache = slotCache();
  std::lock_guard<std::mutex> lock(cache.mutex);
  for (auto it = cache.entries.begin(); it != cache.entries.end(); ++it) {
    if (sameQuery(*it, source, parse, languages)) {
      slots = it->slots;
      cache.entries.splice(cache.entries.begin(), cache.entries, it);
      return true;
    }
  }
  return false;
}

size_t retainedBytes(const CachedSlots &entry) {
  size_t bytes = sizeof(entry) + entry.source.capacity() +
                 entry.languages.capacity() * sizeof(std::string) + entry.slots.capacity() * sizeof(FenceSlot);
  for (const auto &language : entry.languages) {
    bytes += language.capacity();
  }
  for (const auto &slot : entry.slots) {
    bytes += slot.language.capacity() + slot.meta.capacity() + slot.code.capacity() * sizeof(char16_t);
  }
  return bytes;
}

void cacheSlots(const std::string &source, ParseOptions parse, const std::vector<std::string> &languages,
                const std::vector<FenceSlot> &slots) {
  // An oversized key cannot fit even before accounting for its answer.
  if (source.size() > SlotCache::kByteCapacity) {
    return;
  }
  CachedSlots entry{source, parse.flags, languages, slots};
  entry.bytes = retainedBytes(entry);
  if (entry.bytes > SlotCache::kByteCapacity) {
    return;
  }

  SlotCache &cache = slotCache();
  std::lock_guard<std::mutex> lock(cache.mutex);
  for (auto it = cache.entries.begin(); it != cache.entries.end(); ++it) {
    // Concurrent workers may have answered the same query while we parsed.
    if (sameQuery(*it, source, parse, languages)) {
      cache.entries.splice(cache.entries.begin(), cache.entries, it);
      return;
    }
  }
  cache.entries.push_front(std::move(entry));
  cache.bytes += cache.entries.front().bytes;
  while (cache.entries.size() > SlotCache::kCapacity || cache.bytes > SlotCache::kByteCapacity) {
    cache.bytes -= cache.entries.back().bytes;
    cache.entries.pop_back();
  }
}

bool preparedSlots(const std::string &source, ParseOptions parse, const std::vector<std::string> &languages,
                   std::vector<FenceSlot> &slots) {
  const auto prepared = PreparedDocument::completed(source, parse);
  if (!prepared) {
    return false;
  }
  if (prepared->result().status == ParseStatus::Ok && prepared->result().document) {
    slots = fenceSlots(*prepared->result().document, languages, true);
  }
  return true;
}

} // namespace

std::vector<FenceSlot> parsedFenceSlots(const std::string &source, ParseOptions parse,
                                        const std::vector<std::string> &languages) {
  if (languages.empty()) {
    return {};
  }
  const ParseResult result = parseMarkdown(source, parse);
  if (result.status != ParseStatus::Ok || !result.document) {
    return {};
  }
  const auto count =
      static_cast<uint32_t>(std::min<size_t>(result.document->blocks.size(), std::numeric_limits<uint32_t>::max()));
  return fenceSlots(*result.document, languages, true, count);
}

ControlledSlots controlledFenceSlots(const std::string &source, ParseOptions parse,
                                     const std::vector<std::string> &languages) {
  ControlledSlots answer;
  if (languages.empty() || cachedSlots(source, parse, languages, answer.slots)) {
    return answer;
  }
  if (!preparedSlots(source, parse, languages, answer.slots)) {
    if (source.size() > kSyncSlotBytes) {
      answer.pending = true;
      return answer;
    }
    answer.slots = parsedFenceSlots(source, parse, languages);
  }
  cacheSlots(source, parse, languages, answer.slots);
  return answer;
}

std::vector<FenceSlot> resolvedFenceSlots(const std::string &source, ParseOptions parse,
                                          const std::vector<std::string> &languages) {
  std::vector<FenceSlot> slots;
  if (languages.empty() || cachedSlots(source, parse, languages, slots)) {
    return slots;
  }
  if (!preparedSlots(source, parse, languages, slots)) {
    slots = parsedFenceSlots(source, parse, languages);
  }
  cacheSlots(source, parse, languages, slots);
  return slots;
}

void clearControlledSlotCache() {
  SlotCache &cache = slotCache();
  std::lock_guard<std::mutex> lock(cache.mutex);
  cache.entries.clear();
  cache.bytes = 0;
}

} // namespace downstream
