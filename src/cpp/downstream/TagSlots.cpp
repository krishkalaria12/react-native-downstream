#include "TagSlots.h"

#include <cctype>
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
std::string trimBlankEdgeLines(const std::string &source, size_t start, size_t end) {
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
  return end > start ? source.substr(start, end - start) : std::string();
}

} // namespace

std::vector<TagSlot> tagSlots(const Document &document, const std::string &source,
                              const std::vector<std::string> &names, bool live) {
  return tagSlots(document, source, names, live, document.settledBlocks);
}

std::vector<TagSlot> tagSlots(const Document &document, const std::string &source,
                              const std::vector<std::string> &names, bool live, uint32_t settledBlocks) {
  if (names.empty()) {
    return {};
  }
  std::unordered_set<std::string> wanted;
  wanted.reserve(names.size());
  for (const auto &name : names) {
    wanted.insert(lowerAscii(name));
  }

  std::vector<TagSlot> slots;
  // One pass over the blocks, tracking the open custom container: a container
  // is reported once, at its first block, with the whole range it holds.
  int32_t openContainer = -1;
  size_t rangeStart = 0;
  for (size_t index = 0; index <= document.blocks.size(); index++) {
    int32_t innermost = -1;
    if (index < document.blocks.size()) {
      const Block &block = document.blocks[index];
      for (auto frame = block.containers.rbegin(); frame != block.containers.rend(); ++frame) {
        if (frame->kind == ContainerKind::Custom) {
          innermost = static_cast<int32_t>(frame->id);
          break;
        }
      }
    }
    if (innermost == openContainer) {
      continue;
    }
    if (openContainer >= 0) {
      // The container just ended: report it if its tag is wanted. The tag
      // index comes from the range's own first block, so a nested container
      // names its own tag rather than its parent's.
      const Block &first = document.blocks[rangeStart];
      int32_t firstTag = -1;
      for (auto frame = first.containers.rbegin(); frame != first.containers.rend(); ++frame) {
        if (frame->kind == ContainerKind::Custom && static_cast<int32_t>(frame->id) == openContainer) {
          firstTag = frame->tag;
          break;
        }
      }
      if (firstTag >= 0 && static_cast<size_t>(firstTag) < document.tags.size()) {
        const Tag &tag = document.tags[static_cast<size_t>(firstTag)];
        if (wanted.find(tag.name) != wanted.end()) {
          TagSlot slot;
          slot.container = static_cast<uint32_t>(openContainer);
          slot.tag = firstTag;
          slot.name = tag.name;
          slot.attributes = tag.attributes;
          slot.blockIndex = rangeStart;
          slot.blockCount = index - rangeStart;
          // The body's source, exactly as written: the tag record carries the
          // byte range between the opening and closing lines, so no
          // re-serialization stands between the writer and the renderer.
          if (tag.bodyEnd > tag.bodyStart && tag.bodyEnd <= source.size()) {
            slot.markdown = trimBlankEdgeLines(source, tag.bodyStart, tag.bodyEnd);
          }
          // A recorded body end means the closer arrived, even when this is
          // the live stream's final container. A finished stream also settles
          // an unclosed container.
          const bool closed = tag.bodyEnd >= tag.bodyStart && tag.bodyEnd != 0;
          const bool settled = index <= settledBlocks;
          slot.incomplete = !closed && (!settled || live);
          slots.push_back(std::move(slot));
        }
      }
    }
    openContainer = innermost;
    rangeStart = index;
  }
  return slots;
}

} // namespace downstream