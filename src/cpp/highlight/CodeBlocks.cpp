#include "CodeBlocks.h"

#include <chrono>
#include <vector>

namespace downstream {
namespace {

/// Total time one committed block may spend finishing itself, across retries.
///
/// A committed block is colored once and then shared, unchanged, by every later
/// snapshot, so unlike a streaming tail it never gets another chance at the text
/// a budget cut it off from. This budget is generous because the work happens
/// once, on a worker, off the frame a reader is waiting on: 250 ms of lexing is
/// far past any size the 64 KiB limit admits on real hardware, so reaching it
/// means the block is pathological rather than merely large.
constexpr double kSettleBudgetMs = 250.0;

void note(const HighlightState &state, HighlightStats *stats) {
  if (stats == nullptr) {
    return;
  }
  stats->blocks++;
  stats->resumes += state.resumed ? 1 : 0;
  stats->stops[static_cast<size_t>(state.lastStop)]++;
}

/// One highlighting pass over a block, without counting it. Both the streaming
/// and the settling entry points are built on this.
void colorOnce(CodeHighlighter &highlighter, Block *block, HighlightState *state, double budgetMs) {
  if (block->kind != BlockKind::CodeBlock || block->language.empty()) {
    // A block with no language and one whose language has no lexer are both
    // plain, and neither creates a lexer.
    block->tokens.clear();
    state->clear();
    state->lastStop = HighlightStop::Skipped;
    block->highlightComplete = true;
    return;
  }
  highlighter.tokenize(block->language, block->text, state, &block->tokens, budgetMs);
  block->highlightComplete = state->lastStop != HighlightStop::Budget;
}

} // namespace

void highlightCodeBlock(CodeHighlighter &highlighter, Block *block, HighlightState *state, double budgetMs,
                        HighlightStats *stats) {
  colorOnce(highlighter, block, state, budgetMs);
  note(*state, stats);
}

void highlightSettledCodeBlock(CodeHighlighter &highlighter, Block *block, HighlightState *state, double budgetMs,
                               HighlightStats *stats) {
  colorOnce(highlighter, block, state, budgetMs);
  // A pass that ran out of budget leaves the rest of the block plain. For a
  // streaming tail that is fine, because the next snapshot resumes from where
  // this one stopped. This block is settled, so nothing would ever resume it and
  // it would stay part-colored forever, which reads as a rendering bug rather
  // than as a limit. Keep passing until it is whole.
  //
  // Resuming is monotone: the state's text is only ever the same text, and each
  // pass lexes forward from the last byte it settled. The retries together
  // therefore cost one full lex of the block, not one lex each.
  uint64_t retries = 0;
  const auto started = std::chrono::steady_clock::now();
  while (state->lastStop == HighlightStop::Budget) {
    const size_t reached = state->finalEnd;
    colorOnce(highlighter, block, state, budgetMs);
    retries++;
    const double spent =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    // A pass that cannot move the block forward would otherwise spin on the same
    // byte until the clock ran out.
    if (state->finalEnd <= reached || spent >= kSettleBudgetMs) {
      break;
    }
  }
  if (stats == nullptr) {
    return;
  }
  // Counted once, as one block, however many passes it took. The retries are
  // visible as resumes so a caller can still see that settling happened.
  stats->blocks++;
  stats->resumes += retries;
  stats->stops[static_cast<size_t>(state->lastStop)]++;
}

bool reuseCompletedCodeTokens(Block *block, const Block &cached) {
  if (!cached.highlightComplete || cached.kind != BlockKind::CodeBlock || block->kind != BlockKind::CodeBlock ||
      cached.language != block->language || cached.text != block->text) {
    return false;
  }
  block->tokens = cached.tokens;
  block->highlightComplete = true;
  return true;
}

void highlightCodeBlocks(CodeHighlighter &highlighter, std::vector<Block> *blocks, double budgetMs,
                         HighlightStats *stats) {
  HighlightState state;
  for (Block &block : *blocks) {
    if (block.kind == BlockKind::CodeBlock && !block.language.empty()) {
      highlightCodeBlock(highlighter, &block, &state, budgetMs, stats);
    }
  }
}

void highlightSettledCodeBlocks(CodeHighlighter &highlighter, std::vector<Block> *blocks, double budgetMs,
                               HighlightStats *stats) {
  HighlightState state;
  for (Block &block : *blocks) {
    if (block.kind == BlockKind::CodeBlock && !block.language.empty()) {
      highlightSettledCodeBlock(highlighter, &block, &state, budgetMs, stats);
    }
  }
}

namespace {

/// True when any fenced block in `blocks` names a language, which is what
/// decides whether a document needs coloring at all.
bool anyColorable(const std::vector<Block> &blocks) {
  for (const Block &block : blocks) {
    if (block.kind == BlockKind::CodeBlock && !block.language.empty()) {
      return true;
    }
  }
  return false;
}

} // namespace

DocumentHandle highlightCodeBlocks(CodeHighlighter &highlighter, DocumentHandle document, double budgetMs,
                                   HighlightStats *stats) {
  if (!document || !anyColorable(document->blocks.tail())) {
    return document;
  }
  // A parse result is immutable, so the tokens go into a copy. Copying a
  // Document copies its block tail and shares the rest, which is what a
  // snapshot of a stream does anyway.
  auto colored = std::make_shared<Document>(*document);
  highlightCodeBlocks(highlighter, &colored->blocks.tail(), budgetMs, stats);
  return colored;
}

DocumentHandle highlightSettledCodeBlocks(CodeHighlighter &highlighter, DocumentHandle document, double budgetMs,
                                          HighlightStats *stats, const DocumentHandle &previous) {
  if (!document || !anyColorable(document->blocks.tail())) {
    return document;
  }
  auto colored = std::make_shared<Document>(*document);
  HighlightState state;
  auto &blocks = colored->blocks.tail();
  const size_t shared = colored->blocks.sharedSize();
  for (size_t index = 0; index < blocks.size(); index++) {
    Block &block = blocks[index];
    if (block.kind != BlockKind::CodeBlock || block.language.empty()) {
      continue;
    }
    const size_t absolute = shared + index;
    if (previous && absolute < previous->blocks.size()) {
      if (reuseCompletedCodeTokens(&block, previous->blocks[absolute])) {
        continue;
      }
    }
    highlightSettledCodeBlock(highlighter, &block, &state, budgetMs, stats);
  }
  return colored;
}

} // namespace downstream
