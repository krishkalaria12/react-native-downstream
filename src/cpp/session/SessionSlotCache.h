#pragma once

#include "FenceSlots.h"
#include "TagSlots.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <deque>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace downstream {

struct SlotQueryWork {
  uint64_t payloadBuildBytes{0};
  uint64_t payloadExportBytes{0};
  uint64_t reusedPayloads{0};
  uint64_t snapshotBuilds{0};
  uint64_t structureBuilds{0};
  uint64_t sourceCaptureBytes{0};
  uint64_t tagBodyCopyBytes{0};
  uint64_t retainedBytes{0};
};

template <typename Slot> struct SlotChanges {
  uint64_t token{0};
  std::vector<uint32_t> order;
  std::vector<std::shared_ptr<const Slot>> changed;
};

inline uint32_t slotKey(const FenceSlot &slot) { return slot.ordinal; }
inline uint32_t slotKey(const TagSlot &slot) { return slot.container; }
inline size_t slotBytes(const FenceSlot &slot) {
  return sizeof(slot) + 64 + slot.code.capacity() * sizeof(char16_t) +
      slot.language.capacity() + slot.meta.capacity();
}
inline size_t slotBytes(const TagSlot &slot) {
  size_t bytes = sizeof(slot) + 64 + slot.markdown.capacity() + slot.name.capacity() +
      (slot.sourceBody ? sizeof(std::string) + 64 + slot.sourceBody->capacity() : 0) +
      slot.blockIdentities.capacity() * sizeof(uint64_t) +
      slot.attributes.capacity() * sizeof(std::pair<std::string, std::string>);
  for (const auto &pair : slot.attributes) bytes += pair.first.capacity() + pair.second.capacity();
  return bytes;
}

// Bytes in the engine representation: fence bodies are UTF-16 and tags UTF-8.
inline size_t slotPayloadBytes(const FenceSlot &slot) {
  return slot.code.size() * sizeof(char16_t) + slot.language.size() + slot.meta.size();
}
inline size_t slotPayloadBytes(const TagSlot &slot) {
  size_t bytes = slot.markdown.size() + slot.name.size();
  for (const auto &pair : slot.attributes) bytes += pair.first.size() + pair.second.size();
  return bytes;
}

/// A bounded history of immutable payloads. Tokens belong to exact native
/// documents and policies, so subscribers can independently apply their delta.
/// No document or canonical source handles are retained by this cache.
template <typename Slot> class SessionSlotCache {
public:
  template <typename Build>
  SlotChanges<Slot> query(uint64_t documentId, uint64_t generation, bool live,
                         std::vector<std::string> names, uint64_t baseToken, Build build) {
    for (auto &name : names) {
      for (auto &unit : name) unit = static_cast<char>(std::tolower(static_cast<unsigned char>(unit)));
    }
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    Entry *current = nullptr;
    const Entry *previous = nullptr;
    const Entry *base = nullptr;
    for (auto &entry : entries_) {
      if (entry.generation != generation || entry.names != names) continue;
      previous = &entry;
      if (entry.documentId == documentId && entry.live == live) current = &entry;
      if (entry.token == baseToken) base = &entry;
    }
    if (!current) {
      Entry entry;
      entry.documentId = documentId;
      entry.generation = generation;
      entry.live = live;
      entry.names = names;
      entry.token = nextToken_.fetch_add(1, std::memory_order_relaxed);
      entry.slots = build(previous ? previous->slots : Payloads{});
      ++work_.snapshotBuilds;
      entry.bytes = sizeof(Entry) + 64 + entry.names.capacity() * sizeof(std::string);
      for (const auto &name : entry.names) entry.bytes += name.capacity();
      entry.bytes += entry.slots.capacity() * sizeof(std::shared_ptr<const Slot>);
      std::unordered_set<const Slot *> previousSlots;
      if (previous) for (const auto &slot : previous->slots) previousSlots.insert(slot.get());
      for (const auto &slot : entry.slots) {
        entry.bytes += slotBytes(*slot);
        if (previousSlots.count(slot.get())) ++work_.reusedPayloads;
        else work_.payloadBuildBytes += slotPayloadBytes(*slot);
      }
      // Compute before insertion/eviction, which may invalidate history pointers.
      auto answer = delta(entry, base);
      if (entry.bytes <= kMaxBytes) {
        entries_.push_back(std::move(entry));
        work_.retainedBytes += entries_.back().bytes;
        while (entries_.size() > kMaxEntries || work_.retainedBytes > kMaxBytes) {
          work_.retainedBytes -= entries_.front().bytes;
          entries_.pop_front();
        }
      }
      return answer;
    }
    return delta(*current, base);
  }

  void clear() { entries_.clear(); work_.retainedBytes = 0; }
  const SlotQueryWork &work() const { return work_; }

private:
  using Payloads = std::vector<std::shared_ptr<const Slot>>;
  struct Entry {
    uint64_t token{0};
    uint64_t documentId{0};
    uint64_t generation{0};
    bool live{false};
    std::vector<std::string> names;
    Payloads slots;
    size_t bytes{0};
  };
  SlotChanges<Slot> delta(const Entry &entry, const Entry *base) {
    SlotChanges<Slot> answer;
    answer.token = entry.token;
    std::unordered_set<const Slot *> baseSlots;
    if (base) for (const auto &slot : base->slots) baseSlots.insert(slot.get());
    for (const auto &slot : entry.slots) {
      answer.order.push_back(slotKey(*slot));
      if (!baseSlots.count(slot.get())) {
        answer.changed.push_back(slot);
        work_.payloadExportBytes += slotPayloadBytes(*slot);
      }
    }
    return answer;
  }
  // Count shared payloads conservatively once per retained entry. This bounds
  // storage even when snapshots change every pass or subscribers use many policies.
  static constexpr size_t kMaxBytes = 8 * 1024 * 1024;
  static constexpr size_t kMaxEntries = 16;
  inline static std::atomic<uint64_t> nextToken_{1};
  std::deque<Entry> entries_;
  SlotQueryWork work_;
};

} // namespace downstream
