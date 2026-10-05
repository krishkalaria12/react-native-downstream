#pragma once

#include "Document.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace downstream {

/// One fenced code block the registered renderers want. The shape is exactly
/// what the renderer component receives as props, and exactly how Streamdown's
/// `CustomRendererProps` is shaped, so a config written for the web adapts by
/// rewriting only the component.

struct FenceSlot {
  /// The block's ordinal among the document's code blocks, as
  /// `SegmentPlan::codeOrdinal` numbers them: stable across append-only
  /// snapshots, and how the React key and the shadow node agree on one block.
  uint32_t ordinal{0};
  size_t blockIndex{0};
  /// The info string's first word as written, cut at `:`. Matched
  /// case-insensitively against the registered languages once, here; a
  /// renderer should still compare case-insensitively per the public contract.
  std::string language;
  /// The info string after its first word, trimmed. "" when there is none;
  /// JavaScript turns it into `undefined`.
  std::string meta;
  /// Exactly what the code block's copy button copies.
  std::u16string code;
  /// True while the fence's content is not final; the exact rule is in the
  /// plan's Streaming section and implemented below.
  bool incomplete{false};
  /// The block's identity, 0 until the block is proven stable (0 for
  /// controlled documents, where nothing is unstable).
  uint64_t identity{0};
};

/// The raw info string's first word, ASCII lower-cased and cut at `:`, the
/// same normalization highlighting uses for a fence's language. Reading it
/// from the raw info means an entity-encoded path such as ```` ```mer&#x61;id ````
/// is never missed by the JavaScript pre-filter's substring search for
/// `mermaid`, and highlighting and slots never disagree about the language.
std::string normalizeFenceLanguage(std::string_view info);

/// Walks the document's code blocks and returns one slot for each fenced block
/// whose normalized language is in `languages` (which are normalized the same
/// way, so a caller passes its list unnormalized). `live` is true for a
/// controlled view, and false for a stream viewed after `finish()`; it feeds
/// the incompleteness rule. The query walks blocks and never parses.
std::vector<FenceSlot> fenceSlots(const Document &document, const std::vector<std::string> &languages,
                                  bool live);

} // namespace downstream
