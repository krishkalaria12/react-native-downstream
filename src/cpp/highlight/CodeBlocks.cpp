#include "CodeBlocks.h"

#include <vector>

namespace downstream {
namespace {

void note(const HighlightState &state, HighlightStats *stats) {
  if (stats == nullptr) {
    return;
  }
  stats->blocks++;
  stats->resumes += state.resumed ? 1 : 0;
  stats->stops[static_cast<size_t>(state.lastStop)]++;
}

} // namespace

void highlightCodeBlock(CodeHighlighter &highlighter, Block *block, HighlightState *state, double budgetMs,
                        HighlightStats *stats) {
  if (block->kind != BlockKind::CodeBlock || block->language.empty()) {
    // A block with no language and one whose language has no lexer are both
    // plain, and neither creates a lexer.
    block->tokens.clear();
    state->clear();
    state->lastStop = HighlightStop::Skipped;
    note(*state, stats);
    return;
  }
  highlighter.tokenize(block->language, block->text, state, &block->tokens, budgetMs);
  note(*state, stats);
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

DocumentHandle highlightCodeBlocks(CodeHighlighter &highlighter, DocumentHandle document, double budgetMs,
                                   HighlightStats *stats) {
  if (!document) {
    return document;
  }
  bool any = false;
  for (const Block &block : document->blocks) {
    if (block.kind == BlockKind::CodeBlock && !block.language.empty()) {
      any = true;
      break;
    }
  }
  if (!any) {
    return document;
  }
  // A parse result is immutable, so the tokens go into a copy. Copying a
  // Document copies its block tail and shares the rest, which is what a
  // snapshot of a stream does anyway.
  auto colored = std::make_shared<Document>(*document);
  highlightCodeBlocks(highlighter, &colored->blocks.tail(), budgetMs, stats);
  return colored;
}

} // namespace downstream
