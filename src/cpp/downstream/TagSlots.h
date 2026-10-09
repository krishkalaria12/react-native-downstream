#pragma once

#include "Document.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace downstream {

/**
 * One custom container the app could draw, as the slot query reports it.
 *
 * Beside `FenceSlot` because the two answer different questions: a fence slot
 * names a code block by ordinal, and a tag slot names a container by id with
 * the source it holds. The query walks blocks and never parses.
 */
struct TagSlot {
  /// The custom container's own id, which is also what the slot child and the
  /// host entry are keyed by.
  uint32_t container{0};
  /// The tag's index into `Document::tags`, for its name and attributes.
  int32_t tag{-1};
  /// The tag's name, lowercased, as the app named it in `allowedTags`.
  std::string name;
  /// The attributes the policy allowed, name then value, both as written.
  std::vector<std::pair<std::string, std::string>> attributes;
  /// The Markdown between the opening and closing tags, exactly as the writer
  /// sent it — not re-serialized, because a tag that is a container is holding
  /// text an app chose to treat as its own rather than as Markdown to parse
  /// again.
  std::string markdown;
  /// The block range the container holds, for keys and debugging. A slot child
  /// is positioned by the measurement's rectangle, not by this, because only
  /// the measurement knows where the range landed.
  size_t blockIndex{0};
  size_t blockCount{0};
  /// True while the closing tag has not arrived; the exact rule is below.
  bool incomplete{false};
  /// Internal reuse provenance; identities must prove the entire body stable.
  std::vector<uint64_t> blockIdentities;
  size_t bodyStart{0};
  size_t bodyEnd{0};
  std::shared_ptr<const std::string> sourceBody;
};

/// The tag slots in a document, for the tag names the app registered.
///
/// Only containers whose tag is in `names` (matched case-insensitively, as tag
/// names compare) are reported: a tag with no renderer draws its children
/// natively, so there is nothing to mount for it. `live` is true for a stream
/// viewed before `finish()`; it feeds the incompleteness rule.
std::vector<TagSlot> tagSlots(const Document &document, const std::string &source,
                              const std::vector<std::string> &names, bool live);

/// The tag slots in a document with an explicit settled prefix. Controlled
/// bare parses use this to treat every block as settled.
std::vector<TagSlot> tagSlots(const Document &document, const std::string &source,
                              const std::vector<std::string> &names, bool live, uint32_t settledBlocks);

/// Source-independent ranges, planned once for all subscribers of a snapshot.
struct TagSlotRange {
  uint32_t container{0};
  int32_t tag{-1};
  size_t blockIndex{0};
  size_t blockCount{0};
};
std::vector<TagSlotRange> planTagSlots(const Document &document);
using TagSourceBodies = std::vector<std::shared_ptr<const std::string>>;
std::string tagSourceBody(std::string_view source, size_t start, size_t end);

std::vector<std::shared_ptr<const TagSlot>> sharedTagSlots(
    const Document &document, const std::string &source, const std::vector<std::string> &names, bool live,
    const std::vector<std::shared_ptr<const TagSlot>> &previous,
    const std::vector<TagSlotRange> *structure = nullptr, const TagSourceBodies *bodies = nullptr);

} // namespace downstream