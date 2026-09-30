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
///
/// One pass only. A pass that runs out of budget leaves the rest of the block
/// plain, which is what a streaming tail wants: the next snapshot resumes it.
/// Use `highlightSettledCodeBlock` for a block that will never be revisited.
void highlightCodeBlock(CodeHighlighter &highlighter, Block *block, HighlightState *state, double budgetMs,
                        HighlightStats *stats);

/// Colors one fenced code block that is settled and will not grow again, such as
/// a committed block that every later snapshot shares.
///
/// Unlike `highlightCodeBlock` this keeps passing until the block is whole, so a
/// block that is too large for one pass still ends up fully colored instead of
/// staying part-colored with no way to recover. Retrying resumes where the last
/// pass stopped, so the passes together cost one lex of the block. Retrying is
/// bounded by time and by forward progress, and a block that still cannot be
/// finished is left with whatever it reached rather than spun on.
///
/// The block is counted once however many passes it takes.
void highlightSettledCodeBlock(CodeHighlighter &highlighter, Block *block, HighlightState *state, double budgetMs,
                               HighlightStats *stats);

/// Copies completed tokens when both code blocks have identical language and
/// text. Returns false for a changed block or an unfinished highlighting pass.
bool reuseCompletedCodeTokens(Block *block, const Block &cached);

/// Colors every fenced code block in `blocks`. One state is reused across the
/// blocks, so consecutive blocks of the same language share a configured lexer
/// and its word lists are parsed once. One pass per block, so a block too large
/// for the budget stays plain; see `highlightSettledCodeBlocks` for blocks that
/// will not be revisited.
void highlightCodeBlocks(CodeHighlighter &highlighter, std::vector<Block> *blocks, double budgetMs,
                         HighlightStats *stats);

/// Colors every fenced code block in `blocks`, each settled within this call.
/// Use this when none of the blocks will grow again, so a block the budget
/// cannot finish in one pass is finished here rather than left part-colored.
void highlightSettledCodeBlocks(CodeHighlighter &highlighter, std::vector<Block> *blocks, double budgetMs,
                                HighlightStats *stats);

/// Colors every fenced code block in `document` and returns a handle carrying
/// the tokens. Returns `document` itself when no code block in its tail names a
/// language, so a prose-only source allocates nothing and creates no lexer.
DocumentHandle highlightCodeBlocks(CodeHighlighter &highlighter, DocumentHandle document, double budgetMs,
                                   HighlightStats *stats);

/// As `highlightCodeBlocks`, with each block settled within this call.
/// Completed tokens from `previous` are reused for code at the same position
/// with identical language and text. Budget-cut tokens are always retried.
DocumentHandle highlightSettledCodeBlocks(CodeHighlighter &highlighter, DocumentHandle document, double budgetMs,
                                          HighlightStats *stats, const DocumentHandle &previous = {});

} // namespace downstream
