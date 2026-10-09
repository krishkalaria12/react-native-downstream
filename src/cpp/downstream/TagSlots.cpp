#include "TagSlots.h"
#include "SegmentPlan.h"

#include <cctype>
#include <unordered_map>
#include <unordered_set>

namespace downstream {

namespace {

/// Lowercases ASCII in place, because tag names compare case-insensitively and
/// the policy already stores them that way.
std::string lowerAscii(std::string text) {
  for (char &unit : text) {
    unit = static_cast<char>(std::tolower(static_cast<unsigned char>(unit)));
  }
  return text;
}

/// Trims blank lines from either edge of a byte range.
///
/// Leading and trailing blank lines are what separate a body from its tags, not
/// content: the renderer wants the body's own first and last lines. Only
/// whitespace-only lines go; indented content stays exactly as written.
std::string trimBlankEdgeLines(std::string_view source, size_t start, size_t end) {
  while (start < end) {
    const size_t lineEnd = std::min(source.find('\n', start), end);
    size_t content = start;
    while (content < lineEnd &&
           (source[content] == ' ' || source[content] == '\t' || source[content] == '\r')) {
      content++;
    }
    if (content < lineEnd) {
      break;
    }
    start = lineEnd < end ? lineEnd + 1 : end;
  }
  while (end > start) {
    size_t lineStart = start;
    const size_t found = source.rfind('\n', end - 1);
    if (found != std::string::npos && found >= start) {
      lineStart = found + 1;
    }
    size_t content = end;
    while (content > lineStart &&
           (source[content - 1] == ' ' || source[content - 1] == '\t' || source[content - 1] == '\r' ||
            source[content - 1] == '\n')) {
      content--;
    }
    if (content > lineStart) {
      end = content;
      break;
    }
    end = lineStart > start ? lineStart - 1 : start;
    if (end > start && source[end - 1] == '\r') {
      end--;
    }
  }
  return end > start ? std::string(source.substr(start, end - start)) : std::string();
}

} // namespace

std::string tagSourceBody(std::string_view source, size_t start, size_t end) {
  return end > start && end <= source.size() ? trimBlankEdgeLines(source, start, end) : std::string{};
}

std::vector<TagSlotRange> planTagSlots(const Document &document) {
  struct ContainerRange { size_t from; size_t to; size_t count; };
  std::unordered_map<int32_t, ContainerRange> ranges;
  for (size_t index = 0; index < document.blocks.size(); ++index) {
    for (const Container &frame : document.blocks[index].containers) {
      if (frame.kind != ContainerKind::Custom) continue;
      const int32_t id = static_cast<int32_t>(frame.id);
      auto inserted = ranges.emplace(id, ContainerRange{index, index + 1, 0});
      inserted.first->second.to = index + 1;
      ++inserted.first->second.count;
    }
  }
  std::unordered_set<int32_t> hostable;
  for (const SegmentPlan &segment : planSegments(document)) {
    if (!segment.isCustom()) continue;
    const auto found = ranges.find(segment.customContainer);
    if (found != ranges.end() && found->second.from == segment.from && found->second.to == segment.to &&
        found->second.count == segment.to - segment.from) hostable.insert(segment.customContainer);
  }
  std::vector<TagSlotRange> result;
  int32_t openContainer = -1;
  size_t rangeStart = 0;
  for (size_t index = 0; index <= document.blocks.size(); ++index) {
    int32_t innermost = -1;
    if (index < document.blocks.size()) {
      for (auto frame = document.blocks[index].containers.rbegin();
           frame != document.blocks[index].containers.rend(); ++frame) {
        if (frame->kind == ContainerKind::Custom) { innermost = static_cast<int32_t>(frame->id); break; }
      }
    }
    if (innermost == openContainer) continue;
    if (openContainer >= 0 && hostable.count(openContainer)) {
      const Block &first = document.blocks[rangeStart];
      for (auto frame = first.containers.rbegin(); frame != first.containers.rend(); ++frame) {
        if (frame->kind != ContainerKind::Custom || static_cast<int32_t>(frame->id) != openContainer) continue;
        if (frame->tag >= 0 && static_cast<size_t>(frame->tag) < document.tags.size()) {
          result.push_back({static_cast<uint32_t>(openContainer), frame->tag, rangeStart, index - rangeStart});
        }
        break;
      }
    }
    openContainer = innermost;
    rangeStart = index;
  }
  return result;
}

namespace {
template <typename Emit, typename Retain>
void visitTagSlots(const Document &document, const std::string &source, const std::vector<std::string> &names,
                   bool live, uint32_t settledBlocks, const std::vector<std::shared_ptr<const TagSlot>> &previous,
                   const std::vector<TagSlotRange> *structure, const TagSourceBodies *bodies,
                   Emit emit, Retain retain) {
  if (names.empty()) return;
  std::unordered_set<std::string> wanted;
  for (const auto &name : names) wanted.insert(lowerAscii(name));
  const auto planned = structure ? std::vector<TagSlotRange>{} : planTagSlots(document);
  const auto &ranges = structure ? *structure : planned;
  std::unordered_map<uint32_t, std::shared_ptr<const TagSlot>> reused;
  for (const auto &slot : previous) reused.emplace(slot->container, slot);
  for (const auto &range : ranges) {
    const Tag &tag = document.tags[range.tag];
    if (!wanted.count(tag.name)) continue;
    const bool closed = tag.bodyEnd >= tag.bodyStart && tag.bodyEnd != 0;
    const bool incomplete = !closed && (range.blockIndex + range.blockCount > settledBlocks || live);
    std::vector<uint64_t> identities;
    bool stable = closed;
    for (size_t at = range.blockIndex; at < range.blockIndex + range.blockCount; ++at) {
      identities.push_back(document.blocks[at].identity);
      stable = stable && document.blocks[at].identity != 0;
    }
    const auto sourceBody = bodies && static_cast<size_t>(range.tag) < bodies->size() ? (*bodies)[range.tag] : nullptr;
    const auto old = reused.find(range.container);
    if (old != reused.end() &&
        ((stable && old->second->blockIdentities == identities) ||
         (sourceBody && sourceBody == old->second->sourceBody)) &&
        old->second->bodyStart == tag.bodyStart && old->second->bodyEnd == tag.bodyEnd &&
        old->second->name == tag.name && old->second->tag == range.tag &&
        old->second->attributes == tag.attributes && old->second->blockIndex == range.blockIndex &&
        old->second->blockCount == range.blockCount && old->second->incomplete == incomplete) {
      retain(old->second);
      continue;
    }
    TagSlot slot;
    slot.blockIdentities = std::move(identities);
    slot.bodyStart = tag.bodyStart;
    slot.bodyEnd = tag.bodyEnd;
    slot.sourceBody = sourceBody;
    slot.container = range.container;
    slot.tag = range.tag;
    slot.name = tag.name;
    slot.attributes = tag.attributes;
    slot.blockIndex = range.blockIndex;
    slot.blockCount = range.blockCount;
    slot.markdown = sourceBody ? *sourceBody : tagSourceBody(source, tag.bodyStart, tag.bodyEnd);
    slot.incomplete = incomplete;
    emit(std::move(slot));
  }
}

} // namespace

std::vector<TagSlot> tagSlots(const Document &document, const std::string &source,
                              const std::vector<std::string> &names, bool live) {
  return tagSlots(document, source, names, live, document.settledBlocks);
}

std::vector<TagSlot> tagSlots(const Document &document, const std::string &source,
                              const std::vector<std::string> &names, bool live, uint32_t settledBlocks) {
  std::vector<TagSlot> slots;
  visitTagSlots(document, source, names, live, settledBlocks, {}, nullptr, nullptr,
      [&](TagSlot slot) { slots.push_back(std::move(slot)); }, [](const auto &) {});
  return slots;
}

std::vector<std::shared_ptr<const TagSlot>> sharedTagSlots(
    const Document &document, const std::string &source, const std::vector<std::string> &names, bool live,
    const std::vector<std::shared_ptr<const TagSlot>> &previous, const std::vector<TagSlotRange> *structure,
    const TagSourceBodies *bodies) {
  std::vector<std::shared_ptr<const TagSlot>> slots;
  visitTagSlots(document, source, names, live, document.settledBlocks, previous, structure, bodies,
      [&](TagSlot slot) { slots.push_back(std::make_shared<const TagSlot>(std::move(slot))); },
      [&](const auto &slot) { slots.push_back(slot); });
  return slots;
}

} // namespace downstream
