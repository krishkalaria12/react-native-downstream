#pragma once

#include "CodeHighlighter.h"
#include "Document.h"

#include <cstdint>
#include <memory>

namespace downstream {

/// What one set of highlighting passes did, for the parser's counters. Callers
/// that do not count pass null.
struct HighlightStats {
  /// Code blocks a pass colored, including the ones it had to leave plain.
  uint64_t blocks{0};
  /// Passes that continued the previous one rather than starting over.
  uint64_t resumes{0};
  /// Passes by why they stopped, indexed by HighlightStop.
  uint64_t stops[kHighlightStopCount]{};
};

/// Colors one fenced code block in place, using `state` so a block that is still
/// growing only pays for its last line. The state is reset when the block's
/// text or language is not what it was built from, so a caller may hand it any
/// block in any order.
void highlightCodeBlock(CodeHighlighter &highlighter, Block *block, HighlightState *state, double budgetMs,
                        HighlightStats *stats);

/// Colors every fenced code block in `blocks`. One state is reused across the
/// blocks, so consecutive blocks of the same language share a configured lexer
/// and its word lists are parsed once.
void highlightCodeBlocks(CodeHighlighter &highlighter, std::vector<Block> *blocks, double budgetMs,
                         HighlightStats *stats);

/// Colors every fenced code block in `document` and returns a handle carrying
/// the tokens. Returns `document` itself when it has no code block with a
/// language, so a prose-only source allocates nothing and creates no lexer.
DocumentHandle highlightCodeBlocks(CodeHighlighter &highlighter, DocumentHandle document, double budgetMs,
                                   HighlightStats *stats);

} // namespace downstream
