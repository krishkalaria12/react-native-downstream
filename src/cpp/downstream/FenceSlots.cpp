#include "FenceSlots.h"

#include "Export.h"

#include <algorithm>
#include <cctype>
#include <unordered_set>

namespace downstream {

namespace {

/// The info string's first word, trimmed of surrounding whitespace by
/// character class: whitespace separates words; a `:` cuts the word.
std::string_view firstInfoWord(std::string_view info) {
  const size_t start = info.find_first_not_of(" \t\r");
  if (start == std::string_view::npos) {
    return {};
  }
  const size_t end = info.find_first_of(" \t\r", start);
  std::string_view word = info.substr(start, end == std::string_view::npos ? end : end - start);
  const size_t colon = word.find(':');
  return colon == std::string_view::npos ? word : word.substr(0, colon);
}

/// The info string with its first word stripped, and the inter-word
/// whitespace removed. "" when the fence names no meta.
std::string rawMeta(std::string_view info) {
  const size_t start = info.find_first_not_of(" \t\r");
  if (start == std::string_view::npos) {
    return {};
  }
  const size_t wordEnd = info.find_first_of(" \t\r", start);
  if (wordEnd == std::string_view::npos) {
    return {};
  }
  const size_t metaStart = info.find_first_not_of(" \t\r", wordEnd);
  if (metaStart == std::string_view::npos) {
    return {};
  }
  size_t metaEnd = info.find_last_not_of(" \t\r") + 1;
  return std::string(info.substr(metaStart, metaEnd - metaStart));
}

} // namespace

std::string normalizeFenceLanguage(std::string_view info) {
  std::string_view word = firstInfoWord(info);
  std::string lowered;
  lowered.reserve(word.size());
  for (char c : word) {
    lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  return lowered;
}

std::vector<FenceSlot> fenceSlots(const Document &document, const std::vector<std::string> &languages,
                                  bool live) {
  return fenceSlots(document, languages, live, document.settledBlocks);
}

std::vector<FenceSlot> fenceSlots(const Document &document, const std::vector<std::string> &languages,
                                  bool live, uint32_t settledBlocks) {
  if (languages.empty()) {
    return {};
  }
  std::unordered_set<std::string> wanted;
  wanted.reserve(languages.size());
  for (const auto &language : languages) {
    wanted.insert(normalizeFenceLanguage(language));
  }

  std::vector<FenceSlot> slots;
  uint32_t ordinal = 0;
  for (size_t index = 0; index < document.blocks.size(); index++) {
    const Block &block = document.blocks[index];
    if (block.kind != BlockKind::CodeBlock) {
      continue;
    }
    const uint32_t thisOrdinal = ordinal++;
    // Indented blocks have no fence and no language, so they are never
    // candidates; they still count toward the ordinal.
    if (block.fenceMarker == 0) {
      continue;
    }
    const auto word = normalizeFenceLanguage(block.info);
    if (word.empty() || wanted.find(word) == wanted.end()) {
      continue;
    }

    FenceSlot slot;
    slot.ordinal = thisOrdinal;
    slot.blockIndex = index;
    // The language and meta reach the renderer exactly as the info string
    // gives them, and never the entity-decoded `language` field: md4c decodes
    // `mer&#x61;id` before the renderer can ever read it, so reads come from
    // the raw info either way.
    slot.language = std::string(firstInfoWord(block.info));
    slot.meta = rawMeta(block.info);
    slot.code = exportCodeText(document, index);
    const bool settled = index < settledBlocks;
    const bool last = index + 1 == document.blocks.size();
    slot.incomplete = !settled || (last && !block.fenceClosed && live);
    slot.identity = block.identity;
    slots.push_back(std::move(slot));
  }

  return slots;
}

} // namespace downstream
