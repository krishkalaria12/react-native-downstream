#include "MathBlocks.h"

#include <memory>

namespace downstream {

namespace {

/// True when `prefix` is a prefix of `text`.
///
/// The last-good-layout test, and deliberately byte-prefix rather than
/// token-prefix: a reader watching `\frac{a}{` grow wants the layout of
/// `\frac{a}`, and wants it for `\frac{a}{b` too. A test that demanded the new
/// text to be a prefix of the old one would find a layout for text the reader
/// has not reached yet, which is worse than none.
bool isPrefixOf(std::string_view prefix, std::string_view text) {
  return prefix.size() <= text.size() && text.compare(0, prefix.size(), prefix) == 0;
}

/**
 * The layout a still-arriving formula keeps showing.
 *
 * The most recent previous layout whose TeX is a prefix of the new one, which
 * is the longest formula the reader has already seen laid out. Null when there
 * is no such thing, in which case the span shows its literal in the run's own
 * color — not the error color, because a formula that has not finished is not
 * an error.
 */
MathLayout::Handle lastGoodLayout(const MathSpan &span, const Document &previous, bool *found) {
  *found = false;
  if (previous.blocks.empty()) {
    return nullptr;
  }
  for (size_t i = previous.blocks.size(); i > 0; i--) {
    const Block &block = previous.blocks[i - 1];
    for (auto it = block.math.rbegin(); it != block.math.rend(); ++it) {
      if (it->start != span.start || it->end != span.end || it->display != span.display) {
        continue;
      }
      if (isPrefixOf(it->tex, span.tex) && it->layout) {
        *found = true;
        return it->layout;
      }
    }
  }
  return nullptr;
}

/// Lays out one span in place, applying the caps before the engine is reached.
void layoutSpan(MathSpan *span, bool settled, const Document *previous, MathLayoutStats *stats) {
  if (span->layout) {
    // Already laid out, which is the common case for a committed block that
    // every later snapshot shares.
    return;
  }
  if (span->tex.size() > math_limits::kMaxTexBytes) {
    if (stats != nullptr) {
      stats->skippedTooLarge++;
      stats->laidOut++;
    }
    span->error = "The formula is longer than this renderer will lay out.";
    return;
  }
  if (!MathEngine::available()) {
    // No engine is a normal state, not a failure: every formula keeps its
    // literal and the view warns once, in development, naming the package.
    return;
  }
  std::string error;
  MathLayout::Handle handle = MathEngine::layout(span->tex, span->display, &error);
  if (handle) {
    span->layout = std::move(handle);
    span->error.clear();
    if (stats != nullptr) {
      stats->laidOut++;
    }
    return;
  }
  if (!settled) {
    bool kept = false;
    if (previous != nullptr) {
      MathLayout::Handle good = lastGoodLayout(*span, *previous, &kept);
      if (kept) {
        span->layout = std::move(good);
        if (stats != nullptr) {
          stats->lastGoodKept++;
        }
        return;
      }
    }
    // Nothing to keep and nothing wrong yet. The span draws its literal in the
    // run's own color, and an error is neither recorded nor reported: a
    // formula that is still arriving is not a formula that failed.
    return;
  }
  span->error = error.empty() ? "The formula could not be laid out." : std::move(error);
  if (stats != nullptr) {
    stats->failed++;
    stats->laidOut++;
  }
}

/// True when the block is settled, so a failure on it is a real failure rather
/// than one that a closing delimiter would fix.
bool settledBlock(const Document &document, size_t index) { return document.isSettled(index + 1); }

} // namespace

uint64_t countFormulas(const Document &document) {
  uint64_t total = 0;
  for (const Block &block : document.blocks) {
    total += block.math.size();
  }
  return total;
}

uint64_t layoutMathInBlock(Block *block, MathLayoutStats *stats) {
  if (block == nullptr || block->math.empty()) {
    return 0;
  }
  uint64_t before = 0;
  if (stats != nullptr) {
    before = stats->laidOut;
  }
  for (MathSpan &span : block->math) {
    layoutSpan(&span, /*settled=*/false, nullptr, stats);
  }
  return stats != nullptr ? stats->laidOut - before : 0;
}

void layoutMathTail(std::vector<Block> *tail, const Document *previous, MathLayoutStats *stats) {
  if (tail == nullptr || tail->empty()) {
    return;
  }
  // The last math block may still grow, so its formulas are the unsettled ones.
  size_t last = tail->size();
  while (last > 0 && (*tail)[last - 1].kind != BlockKind::MathBlock) {
    last--;
  }
  for (size_t index = 0; index < last; index++) {
    Block &block = (*tail)[index];
    if (block.kind != BlockKind::MathBlock) {
      continue;
    }
    for (MathSpan &span : block.math) {
      layoutSpan(&span, /*settled=*/true, previous, stats);
    }
  }
  for (size_t index = last; index < tail->size(); index++) {
    Block &block = (*tail)[index];
    if (block.kind != BlockKind::MathBlock) {
      continue;
    }
    for (MathSpan &span : block.math) {
      layoutSpan(&span, /*settled=*/false, previous, stats);
    }
  }
}

DocumentHandle layoutMath(DocumentHandle document, const DocumentHandle &previous, MathLayoutStats *stats) {
  if (!document) {
    return document;
  }
  auto &tail = document->blocks.tail();
  bool any = false;
  for (const Block &block : tail) {
    if (!block.math.empty()) {
      any = true;
      break;
    }
  }
  if (!any) {
    // A document with no formula in its tail returns the same handle, so a
    // prose-only source allocates nothing and calls no engine. The same shape
    // `highlightCodeBlocks` uses.
    return document;
  }

  // The per-document cap is applied before the first call rather than per block,
  // so a document with 600 formulas does not lay out the first 512 and then
  // stop: it lays out none of them, because a reader who pastes a 600-formula
  // answer is better served by every formula's source than by half the answer
  // typeset.
  const uint64_t total = countFormulas(*document);
  const bool overCap = total > math_limits::kMaxFormulas;

  // A parse result is immutable, so the layouts go into a copy. Copying a
  // Document copies its block tail and shares the rest, which is what publishing
  // a stream snapshot does anyway.
  auto laid = std::make_shared<Document>(*document);
  const size_t shared = laid->blocks.sharedSize();
  for (size_t index = 0; index < tail.size(); index++) {
    Block &block = laid->blocks.tail()[index];
    if (block.math.empty()) {
      continue;
    }
    const size_t absolute = shared + index;
    const bool settled = settledBlock(*laid, absolute);
    if (overCap) {
      for (MathSpan &span : block.math) {
        span.layout.reset();
        span.error = "This document has more formulas than this renderer will lay out.";
      }
      if (stats != nullptr) {
        stats->skippedTooLarge += block.math.size();
        stats->laidOut += block.math.size();
      }
      continue;
    }
    for (MathSpan &span : block.math) {
      layoutSpan(&span, settled, previous.get(), stats);
    }
  }
  return laid;
}

} // namespace downstream
