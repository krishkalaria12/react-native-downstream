#include "IncrementalParser.h"

#include "CodeBlocks.h"
#include "Compare.h"
#include "MathBlocks.h"
#include "Scan.h"
#include "Unicode.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>

namespace downstream {
namespace {

using scan::closesFence;
using scan::fenceOpener;
using scan::Fence;
using scan::isBlankLine;
using scan::isFenceLine;
using scan::isMathFenceLine;
using scan::kNoOffset;
using scan::sameBlock;
using scan::startsWith;

constexpr size_t kMaxRetainedTailBytes = 64 * 1024;
constexpr size_t kMaxCommittedBlocks = 8192;
constexpr size_t kMinAttemptGap = 64;
// Smaller regions reparse cheaply as tail, so a commit attempt costs more
// than it saves.
constexpr size_t kMinCommitBytes = 128;
// md4c rewrites these inside code text: line endings, NUL, and tab expansion.
constexpr std::string_view kVerbatimBreakers("\r\0\t", 3);

std::atomic<uint64_t> nextBlockIdentity{1};

/// The region's last line that is not blank, without its line ending.
std::string_view lastNonBlankLine(std::string_view region) {
  size_t end = region.size();
  while (end > 0) {
    const size_t newline = region.rfind('\n', end - 1);
    const size_t start = newline == kNoOffset ? 0 : newline + 1;
    const std::string_view line = region.substr(start, end - start);
    if (!isBlankLine(line)) {
      return line;
    }
    if (start == 0) {
      break;
    }
    end = start - 1;
  }
  return {};
}

/// The region's last line, without its line ending. Regions end with '\n'.
std::string_view lastLine(std::string_view region) {
  const std::string_view body = region.substr(0, region.size() - 1);
  const size_t newline = body.rfind('\n');
  return newline == kNoOffset ? body : body.substr(newline + 1);
}

/// Literal brackets outside code may become a reference link once a later
/// definition arrives.
bool hasUnresolvedBracket(const Block &block) {
  // A code block's and a math block's contents are verbatim, and a reference
  // link cannot be defined inside one, so a bracket in either is not a
  // bracket that can still resolve.
  if (block.kind == BlockKind::CodeBlock || block.kind == BlockKind::HtmlBlock ||
      block.kind == BlockKind::MathBlock) {
    return false;
  }
  for (size_t index = 0; index < block.text.size(); index++) {
    if (block.text[index] != u'[' && block.text[index] != u']') {
      continue;
    }
    const bool inCode = std::any_of(block.runs.begin(), block.runs.end(), [index](const InlineRun &run) {
      return run.start <= index && index < run.end && (run.styles & InlineStyle::Code) != 0;
    });
    if (!inCode) {
      return true;
    }
  }
  return false;
}

/// Checks that md4c has closed the region structurally. References may be
/// allowed for provisional reuse only while the raw dependency scan proves
/// there is no definition or bracket-math opener in the canonical source.
bool provesClosed(std::string_view region, const ParseResult &parsed, bool allowReferences) {
  if (parsed.status != ParseStatus::Ok || parsed.blockSource.size() != parsed.document->blocks.size()) {
    return false;
  }
  // A reference or footnote definition could resolve a later reference.
  if (region.find("]:") != kNoOffset) {
    return false;
  }
  const auto &blocks = parsed.document->blocks;
  uint32_t lastContainer = 0;
  for (const Block &block : blocks) {
    if (!allowReferences && hasUnresolvedBracket(block)) {
      return false;
    }
    for (const Container &container : block.containers) {
      lastContainer = std::max(lastContainer, container.id);
    }
  }
  // A container without blocks, such as an empty list item, can take later lines.
  if (lastContainer != parsed.containerCount) {
    return false;
  }
  if (blocks.empty()) {
    return true;
  }
  const Block &last = blocks.back();
  if (!last.containers.empty()) {
    return false;
  }
  switch (last.kind) {
    case BlockKind::Heading:
    case BlockKind::ThematicBreak:
      return true;
    case BlockKind::CodeBlock:
      // Indented code continues after blank lines; only a fence can close.
      return isFenceLine(lastNonBlankLine(region));
    case BlockKind::MathBlock:
      // A `$$` fence is a fence, so the same rule: a blank line inside an
      // `aligned` environment does not end the formula, and only the closing
      // delimiter does. Without this a commit would land inside a formula.
      return isMathFenceLine(lastNonBlankLine(region)) || isFenceLine(lastNonBlankLine(region));
    case BlockKind::Paragraph:
    case BlockKind::TableRow:
    case BlockKind::HtmlBlock:
    case BlockKind::Image:
      // An image block is a paragraph, so a blank line ends it exactly as it
      // ends the paragraph it was promoted from.
      return isBlankLine(lastLine(region));
  }
  return false;
}

/// True when text appended after the region starts a new block and leaves
/// every region block unchanged.
bool splitsBefore(const ParseResult &parsed, const ParseResult &probed) {
  const auto &before = parsed.document->blocks;
  if (probed.status != ParseStatus::Ok || probed.document->blocks.size() != before.size() + 1) {
    return false;
  }
  const Block &added = probed.document->blocks.back();
  if (added.kind != BlockKind::Paragraph || !added.containers.empty() || added.text != u"x") {
    return false;
  }
  for (size_t i = 0; i < before.size(); i++) {
    if (!sameBlock(before[i], probed.document->blocks[i])) {
      return false;
    }
  }
  return true;
}

void offsetIds(Block &block, uint32_t containers, uint32_t tables, size_t links, size_t images) {
  for (Container &container : block.containers) {
    container.id += containers;
  }
  if (block.table != 0) {
    block.table += tables;
  }
  for (InlineRun &run : block.runs) {
    if (run.link >= 0) {
      run.link += static_cast<int32_t>(links);
    }
  }
  // A block's image indexes are into the document's image list, so a spliced
  // parse moves them the same way it moves a run's link index. Getting this
  // wrong would make a promoted image block name somebody else's picture, which
  // is the kind of bug that only shows up in a long chat.
  for (uint32_t &index : block.images) {
    index += static_cast<uint32_t>(images);
  }
}

void offsetImage(Image &image, int32_t links, uint32_t utf16) {
  // A linked image's press reaches the enclosing link, so the image holds an
  // index into the same list a run's `link` field indexes.
  if (image.link >= 0) {
    image.link += links;
  }
  if (image.range.end > image.range.start) {
    image.range.start += utf16;
    image.range.end += utf16;
  }
}

void offsetLink(Link &link, uint32_t utf16) {
  // An empty range is unknown in a full parse too; keep it unknown.
  if (link.range.end > link.range.start) {
    link.range.start += utf16;
    link.range.end += utf16;
  }
}

} // namespace

void IncrementalParser::append(std::string_view chunk) {
  source_.append(chunk);
}

void IncrementalParser::replace(std::string_view source) {
  invalidateReferencePrefix();
  dependencyScannedBytes_ = 0;
  referenceDependentSource_ = false;
  // Committed blocks stay candidates only while their source is unchanged;
  // the next full parse confirms them against md4c's output.
  if (source.size() < committedBytes_ ||
      source.substr(0, committedBytes_) != std::string_view(source_).substr(0, committedBytes_)) {
    clearCommitted();
  }
  source_.assign(source);
  replaced_ = true;
  openFence_ = false;
  highlight_.clear();
}

void IncrementalParser::reset() {
  source_.clear();
  document_.reset();
  parsedBytes_ = 0;
  replaced_ = false;
  clearCommitted();
  openFence_ = false;
  highlight_.clear();
  previewStats_ = {};
  counters_ = {};
  dependencyScannedBytes_ = 0;
  referenceDependentSource_ = false;
}

size_t IncrementalParser::committedCount() const {
  return committedBlocks_ ? committedBlocks_->size() : 0;
}

size_t IncrementalParser::settledCount() const {
  return referencePrefix_ ? (referencePrefix_->blocks ? referencePrefix_->blocks->size() : 0) : committedCount();
}

void IncrementalParser::invalidateReferencePrefix() {
  if (!referencePrefix_) return;
  const ReferencePrefix checkpoint = std::move(*referencePrefix_);
  referencePrefix_.reset();
  committedBlocks_ = checkpoint.blocks;
  committedLinks_ = checkpoint.links;
  committedImages_ = checkpoint.images;
  committedBytes_ = checkpoint.bytes;
  committedUtf16_ = checkpoint.utf16;
  committedContainers_ = checkpoint.containers;
  committedTables_ = checkpoint.tables;
  openFence_ = false;
  highlight_.clear();
  resetScan();
}

void IncrementalParser::scanReferenceDependencies() {
  if (!referenceDependentSource_) {
    // Both definition parsers in md4c require adjacent literal `]:` bytes.
    // Bracket math can also rewrite earlier source when a late closer arrives,
    // so keep its opener on the conservative path. Include the previous byte
    // because any marker may be split across append calls.
    const size_t from = dependencyScannedBytes_ > 0 ? dependencyScannedBytes_ - 1 : 0;
    const std::string_view added = std::string_view(source_).substr(from);
    referenceDependentSource_ = added.find("]:") != kNoOffset ||
        (parse_.enabled() && parse_.brackets() &&
         (added.find("\\[") != kNoOffset || added.find("\\(") != kNoOffset));
  }
  dependencyScannedBytes_ = source_.size();
  if (referenceDependentSource_ || preview_.enabled()) invalidateReferencePrefix();
}

void IncrementalParser::clearCommitted() {
  referencePrefix_.reset();
  committedBlocks_.reset();
  committedLinks_.reset();
  committedImages_.reset();
  committedBytes_ = 0;
  committedUtf16_ = 0;
  committedContainers_ = 0;
  committedTables_ = 0;
  resetScan();
}

void IncrementalParser::resetScan() {
  scannedBytes_ = 0;
  lastBlankEnd_ = 0;
  nextBlankAttempt_ = 0;
  nextLineAttempt_ = 0;
  lineAttempted_ = 0;
}

static void hideIncompleteTable(Document *document);

const DocumentHandle &IncrementalParser::publish(DocumentHandle document, uint32_t settled) {
  // A trailing table whose rows are still arriving is withheld whole when the
  // mode asks for it: not even its settled rows are shown, because a half-drawn
  // grid is what the mode exists to prevent. Done here rather than in either
  // parse path so full and incremental snapshots agree, and only on a copy: the
  // rows are still in the parse, so they come back — together — as soon as the
  // table completes.
  //
  // The two flags a snapshot carries that its blocks do not: how many leading
  // blocks no later input can change, and whether a stream is still arriving.
  // The second is how an `isAnimating` prop reaches the native rendering. A pass
  // that built its document sets both in place; only a document shared with an
  // earlier snapshot is copied here.
  const bool streaming = !finished_;
  const bool hideTables = streaming && preview_.options().has(remend::PreviewFeature::Tables);
  if (document && (document->settledBlocks != settled || document->streaming != streaming || hideTables)) {
    auto copy = std::make_shared<Document>(*document);
    copy->settledBlocks = settled;
    copy->streaming = streaming;
    if (hideTables) {
      hideIncompleteTable(copy.get());
    }
    document = std::move(copy);
  }
  document_ = std::move(document);
  parsedBytes_ = source_.size();
  replaced_ = false;
  return document_;
}

ParseResult IncrementalParser::fullParse(ParseFallback reason, bool previewable, bool settled) {
  const std::string_view source = source_;
  // A stream that is still open previews on every full parse: before a commit
  // boundary, in full mode, after a reset, and past the tail limit. The pass
  // parses the source itself, so this is one parse when nothing is repaired.
  // Committed blocks lead the parse and are frozen, so a repair cannot restyle
  // one even though this parse covers them.
  ParseResult result;
  if (previewable) {
    const remend::PreviewResult preview = preview_.apply(source, 0, settledCount(), parse_);
    notePreview(preview);
    result = preview.parsed;
  } else {
    result = parseMarkdown(source, parse_);
    counters_.bytesParsed += source.size();
  }
  counters_.fullParses++;
  if (reason != ParseFallback::Count) {
    counters_.fallback[static_cast<size_t>(reason)]++;
  }
  if (result.status != ParseStatus::Ok) {
    return result;
  }
  // Color before anything compares blocks. sameBlock reads a block's tokens, and
  // a freshly parsed block has none until the highlighter has seen it, so
  // comparing first would find every block changed and defeat both the identity
  // carry-over and the shared committed prefix. A full parse replaces the whole
  // document, so the tail's resume state no longer describes anything and its
  // lexer is released.
  //
  // Every block here is whole, and the committed prefix this result is about to
  // be compared against is already colored, so each block settles: a block the
  // budget cannot finish in one pass is finished here. A partially colored
  // committed block would not equal the same block colored in full, and that
  // mismatch alone would drop the shared prefix on the next parse.
  highlight_.clear();
  HighlightStats stats;
  result.document = highlightSettledCodeBlocks(lexillaHighlighter(), result.document, kHighlightBudgetMs, &stats,
                                               replaced_ ? nullptr : document_);
  noteHighlight(stats);
  // Math runs after highlighting and before anything compares blocks, for the
  // same reason: a layout is part of what a block draws, so comparing a block
  // whose formula has not been laid out against one whose formula has would
  // find them different. A MathSpan's own identity excludes its layout, so a
  // block keeps its identity while the pass fills it in.
  MathLayoutStats mathStats;
  result.document = layoutMath(result.document, replaced_ ? nullptr : document_, &mathStats);
  noteMath(mathStats);
  // A full parse has no proof that any particular block is closed, so nothing
  // is settled until the stream finishes. Only a finished stream settles; a
  // controlled document settles in PreparedDocument, the moment it is parsed.
  const uint32_t count = static_cast<uint32_t>(std::min<size_t>(result.document->blocks.size(), UINT32_MAX));
  const uint32_t settledCount = settled ? count : 0;
  if (mode_ == ParseMode::FullParse) {
    carryIdentities(result);
    result.document = publish(result.document, settledCount);
    return result;
  }

  // A fallback reparses everything, but committed blocks keep their shared
  // storage and identity when md4c still produces them unchanged.
  const size_t committed = committedCount();
  const size_t committedLinks = committedLinks_ ? committedLinks_->size() : 0;
  const size_t committedImages = committedImages_ ? committedImages_->size() : 0;
  const Document &parsed = *result.document;
  bool confirmed = committed > 0 && source.size() >= committedBytes_ && parsed.blocks.size() >= committed &&
      parsed.links.size() >= committedLinks && parsed.images.size() >= committedImages;
  for (size_t i = 0; confirmed && i < committed; i++) {
    confirmed = sameBlock((*committedBlocks_)[i], parsed.blocks[i]);
  }
  for (size_t i = 0; confirmed && i < committedLinks; i++) {
    const Link &link = (*committedLinks_)[i];
    confirmed = link.url == parsed.links[i].url && link.range == parsed.links[i].range;
  }
  for (size_t i = 0; confirmed && i < committedImages; i++) {
    confirmed = scan::sameImage((*committedImages_)[i], parsed.images[i]);
  }
  if (confirmed) {
    const auto &blocks = parsed.blocks.tail();
    const auto &links = parsed.links.tail();
    const auto &images = parsed.images.tail();
    auto shared = std::make_shared<Document>();
    shared->id = parsed.id;
    shared->blocks = SharedList<Block>(committedBlocks_, std::vector<Block>(blocks.begin() + committed, blocks.end()));
    shared->links = SharedList<Link>(committedLinks_, std::vector<Link>(links.begin() + committedLinks, links.end()));
    shared->images = SharedList<Image>(committedImages_, std::vector<Image>(images.begin() + committedImages, images.end()));
    shared->settledBlocks = result.document->settledBlocks;
    result.document = shared;
    counters_.cacheHits += committed;
  } else if (committed > 0) {
    clearCommitted();
  }
  // The proven prefix still stands, but lines after it may have changed.
  if (reason == ParseFallback::NonAppend) {
    resetScan();
  }
  result.document = publish(result.document, settledCount);
  watchOpenFence(source, committedBytes_, committedCount());
  return result;
}

void IncrementalParser::noteHighlight(const HighlightStats &stats) {
  counters_.blocksHighlighted += stats.blocks;
  counters_.highlightResumes += stats.resumes;
  for (size_t index = 0; index < kHighlightStopCount; index++) {
    counters_.highlight[index] += stats.stops[index];
  }
}

void IncrementalParser::highlightTail(Document *document) {
  HighlightStats stats;
  std::vector<Block> &blocks = document->blocks.tail();
  // The tail is rebuilt from scratch, but unchanged code keeps its completed
  // tokens. The last code block may still grow, so it gets the state that
  // resumes where the previous snapshot stopped.
  size_t last = blocks.size();
  while (last > 0 &&
         !(blocks[last - 1].kind == BlockKind::CodeBlock && !blocks[last - 1].language.empty())) {
    last--;
  }
  if (last == 0) {
    highlight_.clear();
    return;
  }
  // A local state for the settled blocks in front of it: they share a lexer with
  // each other, and never disturb the growing block's.
  HighlightState settled;
  const size_t shared = document->blocks.sharedSize();
  for (size_t i = 0; i < last; i++) {
    Block &block = blocks[i];
    if (block.kind != BlockKind::CodeBlock || block.language.empty()) {
      continue;
    }
    const size_t absolute = shared + i;
    if (document_ && absolute < document_->blocks.size() &&
        reuseCompletedCodeTokens(&block, document_->blocks[absolute])) {
      continue;
    }
    if (i + 1 == last) {
      highlightCodeBlock(lexillaHighlighter(), &block, &highlight_, kHighlightBudgetMs, &stats);
    } else {
      // Earlier blocks cannot grow. A budget cut must be finished now because
      // no later snapshot can otherwise complete their highlighting.
      highlightSettledCodeBlock(lexillaHighlighter(), &block, &settled, kHighlightBudgetMs, &stats);
    }
  }
  noteHighlight(stats);
}

/**
 * Lays out the tail's formulas, keeping the last good layout for one that is
 * still arriving.
 *
 * Unlike highlighting there is no budget to respect: a formula is one call
 * through a cache, and a streaming snapshot that skipped it would leave a
 * formula showing its source for a frame it did not have to. The pass returns
 * the same handle when the tail has no formula, so a prose-only snapshot costs
 * one scan of the tail's kinds.
 */
void IncrementalParser::layoutTail(Document *document) {
  if (document == nullptr) {
    return;
  }
  MathLayoutStats stats;
  layoutMathTail(&document->blocks.tail(), document_.get(), &stats);
  noteMath(stats);
}

void IncrementalParser::noteMath(const MathLayoutStats &stats) {
  counters_.formulasLaidOut += stats.laidOut;
  counters_.mathLastGoodKept += stats.lastGoodKept;
  counters_.mathSkippedTooLarge += stats.skippedTooLarge;
  counters_.mathFailed += stats.failed;
}

void IncrementalParser::notePreview(const remend::PreviewResult &preview) {
  previewStats_.probes += preview.stats.probes;
  previewStats_.applied += preview.stats.applied;
  previewStats_.refused += preview.stats.refused;
  counters_.bytesParsed += preview.bytesParsed;
}

void IncrementalParser::carryIdentities(ParseResult &result) {
  // Identities describe append-only history; a replaced source starts over.
  const DocumentHandle previous = replaced_ ? nullptr : document_;
  const auto &blocks = result.document->blocks;
  if (!previous || blocks.size() < 2 || result.blockSource.size() != blocks.size()) {
    return;
  }
  // The last block may still grow, so it is never stable.
  const size_t limit = std::min(previous->blocks.size(), blocks.size() - 1);
  size_t stable = 0;
  while (stable < limit && sameBlock(previous->blocks[stable], blocks[stable])) {
    stable++;
  }
  // Like a proven commit, the stable prefix ends after a top-level block that
  // closes any table, so no quote, list, or table continues past it.
  while (stable > 0 &&
         (!blocks[stable - 1].containers.empty() ||
          (blocks[stable - 1].kind == BlockKind::TableRow && blocks[stable].kind == BlockKind::TableRow &&
           blocks[stable].table == blocks[stable - 1].table))) {
    stable--;
  }
  if (stable == 0) {
    return;
  }

  auto document = std::make_shared<Document>(*result.document);
  auto &carried = document->blocks.tail();
  Utf16OffsetMap offsets(source_);
  for (size_t i = 0; i < stable; i++) {
    // A block that stayed unchanged for one update is unlikely to change
    // again. If it does, the next parse gives it a new identity, so an
    // identity always names the same content.
    const uint64_t identity = previous->blocks[i].identity;
    carried[i].identity = identity != 0 ? identity : nextBlockIdentity.fetch_add(1, std::memory_order_relaxed);
    const ByteRange bytes = result.blockSource[i];
    carried[i].sourceRange = {offsets.toUtf16(bytes.start), offsets.toUtf16(bytes.end)};
  }
  result.document = document;
}

ParseResult IncrementalParser::parseTail() {
  const std::string_view source = source_;
  const std::string_view tail = source.substr(committedBytes_);

  // The preview sees only the tail, which is exactly the region this parse
  // rebuilds from scratch. Everything below committedBytes_ lives in storage
  // the repair cannot reach, so no repair can restyle a block a reader has
  // already seen settled.
  const remend::PreviewResult preview = preview_.apply(tail, committedBytes_, 0, parse_);
  notePreview(preview);
  ParseResult result = preview.parsed;
  counters_.tailParses++;
  if (result.status != ParseStatus::Ok) {
    return result;
  }

  const size_t committed = committedCount();
  const size_t committedLinks = committedLinks_ ? committedLinks_->size() : 0;
  const size_t committedImages = committedImages_ ? committedImages_->size() : 0;
  std::vector<Block> blocks(result.document->blocks.begin(), result.document->blocks.end());
  for (Block &block : blocks) {
    offsetIds(block, committedContainers_, committedTables_, committedLinks, committedImages);
  }
  // The tail parse saw an empty document before its first block.
  if (committed > 0 && !blocks.empty()) {
    blocks.front().spacingBefore = BlockSpacing::Normal;
  }
  std::vector<Link> links(result.document->links.begin(), result.document->links.end());
  for (Link &link : links) {
    offsetLink(link, committedUtf16_);
  }
  std::vector<Image> images(result.document->images.begin(), result.document->images.end());
  for (Image &image : images) {
    offsetImage(image, static_cast<int32_t>(committedLinks), committedUtf16_);
  }
  auto combined = std::make_shared<Document>();
  combined->id = result.document->id;
  combined->blocks = SharedList<Block>(committedBlocks_, std::move(blocks));
  combined->links = SharedList<Link>(committedLinks_, std::move(links));
  combined->images = SharedList<Image>(committedImages_, std::move(images));
  // Only the strict prefix is settled. Shared provisional reference blocks
  // remain mutable and are never offered as settled text to controls.
  combined->settledBlocks = static_cast<uint32_t>(std::min<size_t>(settledCount(), UINT32_MAX));
  combined->streaming = !finished_;
  counters_.cacheHits += committed;
  result.document = combined;
  highlightTail(combined.get());
  layoutTail(combined.get());
  result.document = publish(combined, combined->settledBlocks);
  watchOpenFence(source, committedBytes_, committed);
  return result;
}

/**
 * Drops a trailing table that has not completed yet.
 *
 * Only the tail is ever touched: the settled prefix is what md4c proved no
 * later input can change, and the tail is rebuilt on every snapshot anyway, so
 * withholding rows from it cannot lose a reader's text. Links and images that
 * belonged only to the dropped rows go with them, because a press on a link the
 * reader cannot see is a press on nothing.
 *
 * A table is complete when a non-row block follows it or the stream finishes.
 * Anything else — rows still arriving — means the grid is half-drawn, which is
 * exactly what is hidden.
 */
static void hideIncompleteTable(Document *document) {
  const size_t count = document->blocks.size();
  const uint32_t settled = document->settledBlocks;
  size_t keep = count;
  // Trailing rows that no later input can still change are complete enough to
  // keep: only the rows past the settled prefix are still arriving.
  while (keep > 0 && keep > settled && document->blocks[keep - 1].kind == BlockKind::TableRow) {
    keep--;
  }
  if (keep == count) {
    return;
  }
  // The table is withheld whole, not row by row: showing its settled rows while
  // its tail is still arriving is still a half-drawn grid.
  while (keep > 0 && document->blocks[keep - 1].kind == BlockKind::TableRow) {
    keep--;
  }
  int32_t maxLink = -1;
  int32_t maxImage = -1;
  for (size_t index = 0; index < keep; index++) {
    const Block &block = document->blocks[index];
    for (const InlineRun &run : block.runs) {
      maxLink = std::max(maxLink, run.link);
    }
    for (uint32_t image : block.images) {
      maxImage = std::max(maxImage, static_cast<int32_t>(image));
    }
  }
  // Links and images are appended in block order, so everything past the
  // highest index a kept block names belonged to a dropped row. Copied out
  // rather than spliced because the lists only move forward: a forward iterator
  // is all they offer, and the tail is small.
  std::vector<Block> blocks;
  blocks.reserve(keep);
  for (size_t index = 0; index < keep; index++) {
    blocks.push_back(document->blocks[index]);
  }
  std::vector<Link> links;
  for (int32_t index = 0; index <= maxLink; index++) {
    links.push_back(document->links[static_cast<size_t>(index)]);
  }
  std::vector<Image> images;
  for (int32_t index = 0; index <= maxImage; index++) {
    images.push_back(document->images[static_cast<size_t>(index)]);
  }
  document->blocks = SharedList<Block>(document->blocks.prefix(), std::move(blocks));
  document->links = SharedList<Link>(document->links.prefix(), std::move(links));
  document->images = SharedList<Image>(document->images.prefix(), std::move(images));
}

void IncrementalParser::tryCommit() {
  const std::string_view source = source_;
  size_t lineStart = std::max(scannedBytes_, committedBytes_);
  size_t lastLineStart = kNoOffset;
  for (;;) {
    const size_t newline = source.find('\n', lineStart);
    if (newline == kNoOffset) {
      break;
    }
    if (isBlankLine(source.substr(lineStart, newline - lineStart))) {
      lastBlankEnd_ = newline + 1;
    }
    lastLineStart = lineStart;
    lineStart = newline + 1;
  }
  scannedBytes_ = lineStart;
  if (committedCount() >= kMaxCommittedBlocks) {
    return;
  }

  const auto gap = [this](size_t boundary) {
    return std::max(kMinAttemptGap, (boundary - committedBytes_) / 2);
  };
  // A blank line closes most blocks, so it is the likeliest boundary.
  size_t attempted = 0;
  if (lastBlankEnd_ >= committedBytes_ + kMinCommitBytes && lastBlankEnd_ >= nextBlankAttempt_) {
    attempted = lastBlankEnd_;
    if (!commitRegion(lastBlankEnd_)) {
      nextBlankAttempt_ = lastBlankEnd_ + gap(lastBlankEnd_);
    }
  }

  // Headings and closing fences end a block without a blank line. Other
  // line ends are retried only after the region has grown.
  const size_t lineEnd = scannedBytes_;
  if (lineEnd < committedBytes_ + kMinCommitBytes || lineEnd == attempted || lineEnd <= lineAttempted_) {
    return;
  }
  if (lastLineStart == kNoOffset) {
    const size_t newline = source.rfind('\n', lineEnd - 2);
    lastLineStart = newline == kNoOffset || newline < committedBytes_ ? committedBytes_ : newline + 1;
  }
  const std::string_view line = source.substr(lastLineStart, lineEnd - 1 - lastLineStart);
  if (lineEnd >= nextLineAttempt_ || startsWith(line, '#') || startsWith(line, '`') || startsWith(line, '~')) {
    lineAttempted_ = lineEnd;
    if (!commitRegion(lineEnd)) {
      nextLineAttempt_ = lineEnd + gap(lineEnd);
    }
  }
}

bool IncrementalParser::commitRegion(size_t boundary) {
  const std::string_view region = std::string_view(source_).substr(committedBytes_, boundary - committedBytes_);
  counters_.commitAttempts++;
  counters_.bytesParsed += region.size();
  const ParseResult parsed = parseMarkdownAt(region, committedBytes_, parse_);
  const bool allowReferences = !referenceDependentSource_ && !preview_.enabled();
  bool closed = provesClosed(region, parsed, allowReferences) &&
      committedCount() + parsed.document->blocks.size() <= kMaxCommittedBlocks;
  if (closed && !parsed.document->blocks.empty()) {
    const BlockKind kind = parsed.document->blocks.back().kind;
    // Open fences and some HTML blocks run to the end of input.
    if (kind == BlockKind::CodeBlock || kind == BlockKind::HtmlBlock) {
      std::string probe(region);
      probe += "x\n";
      counters_.bytesParsed += probe.size();
      closed = splitsBefore(parsed, parseMarkdownAt(probe, committedBytes_, parse_));
    }
  }
  if (!closed) {
    counters_.rejectedCommits++;
    return false;
  }

  const auto &parsedBlocks = parsed.document->blocks;
  if (!referencePrefix_ && std::any_of(parsedBlocks.begin(), parsedBlocks.end(), hasUnresolvedBracket)) {
    referencePrefix_ = ReferencePrefix{committedBlocks_, committedLinks_, committedImages_, committedBytes_,
                                       committedUtf16_, committedContainers_, committedTables_};
  }
  const size_t committed = committedCount();
  const size_t committedLinks = committedLinks_ ? committedLinks_->size() : 0;
  const size_t committedImages = committedImages_ ? committedImages_->size() : 0;
  Utf16OffsetMap offsets(region);
  // Reused across the region's blocks, so consecutive ones of the same
  // language share a configured lexer.
  HighlightState committedHighlight;
  HighlightStats stats;
  MathLayoutStats committedMath;
  std::vector<Block> blocks;
  blocks.reserve(parsedBlocks.size());
  for (size_t i = 0; i < parsedBlocks.size(); i++) {
    Block block = parsedBlocks[i];
    block.identity = referencePrefix_ ? 0 : nextBlockIdentity.fetch_add(1, std::memory_order_relaxed);
    const ByteRange bytes = parsed.blockSource[i];
    block.sourceRange = {committedUtf16_ + offsets.toUtf16(bytes.start), committedUtf16_ + offsets.toUtf16(bytes.end)};
    offsetIds(block, committedContainers_, committedTables_, committedLinks, committedImages);
    if (i == 0 && committed > 0) {
      block.spacingBefore = BlockSpacing::Normal;
    }
    // A closed code block is colored here from its whole text before sharing.
    // The provisional prefix can be revoked, but while it is shared no later
    // pass revisits its tokens. Complete them now so a budget stop does not
    // leave the rest of the shared block plain forever.
    // A local state keeps this away from the tail's.
    if (block.kind == BlockKind::CodeBlock && !block.language.empty()) {
      highlightSettledCodeBlock(lexillaHighlighter(), &block, &committedHighlight, kHighlightBudgetMs, &stats);
    }
    // A committed formula is closed, inline or display, so it is laid out here
    // from its whole TeX and then shared, unchanged, by every later snapshot. The
    // same argument as highlighting applies: nothing will ever revisit it, and a
    // half-laid-out committed block would not equal the same block laid out in
    // full.
    if (!block.math.empty()) {
      layoutMathInBlock(&block, &committedMath);
    }
    blocks.push_back(std::move(block));
  }
  if (!blocks.empty()) {
    committedBlocks_ = SharedList<Block>::Prefix::extend(committedBlocks_, std::move(blocks));
  }
  if (!parsed.document->links.empty()) {
    std::vector<Link> links(parsed.document->links.begin(), parsed.document->links.end());
    for (Link &link : links) {
      offsetLink(link, committedUtf16_);
    }
    committedLinks_ = SharedList<Link>::Prefix::extend(committedLinks_, std::move(links));
  }
  if (!parsed.document->images.empty()) {
    std::vector<Image> images(parsed.document->images.begin(), parsed.document->images.end());
    for (Image &image : images) {
      offsetImage(image, static_cast<int32_t>(committedLinks), committedUtf16_);
    }
    committedImages_ = SharedList<Image>::Prefix::extend(committedImages_, std::move(images));
  }
  noteHighlight(stats);
  noteMath(committedMath);
  committedBytes_ = boundary;
  committedUtf16_ += offsets.toUtf16(region.size());
  committedContainers_ += parsed.containerCount;
  committedTables_ += parsed.tableCount;
  nextBlankAttempt_ = 0;
  nextLineAttempt_ = 0;
  if (!referencePrefix_) counters_.committedBlocks += parsedBlocks.size();
  return true;
}
void IncrementalParser::watchOpenFence(std::string_view source, size_t from, size_t blocksBefore) {
  openFence_ = false;
  if (mode_ != ParseMode::IncrementalTail || !document_ || document_->blocks.empty()) {
    return;
  }
  const Block &last = document_->blocks.back();
  // A math block counts here for the same reason a code block does: a `$$`
  // fence is a fence, and the open-fence fast path is what lets an `aligned`
  // environment grow a line at a time instead of reparsing the formula for
  // every token. A block with runs is a paragraph, never a fence body.
  if ((last.kind != BlockKind::CodeBlock && last.kind != BlockKind::MathBlock) || !last.containers.empty() ||
      !last.runs.empty()) {
    return;
  }

  std::optional<Fence> open;
  size_t contentStart = 0;
  size_t lineStart = from;
  for (;;) {
    const size_t newline = source.find('\n', lineStart);
    const size_t lineEnd = newline == kNoOffset ? source.size() : newline;
    const std::string_view line = source.substr(lineStart, lineEnd - lineStart);
    if (open) {
      if (closesFence(line, open->marker, open->length)) {
        open.reset();
      }
    } else if (const auto candidate = fenceOpener(line, parse_.enabled() && last.kind == BlockKind::MathBlock)) {
      // An opener still being typed can change its info string, and a `$$` line
      // with no newline yet is not yet a fence.
      if (newline == kNoOffset) {
        return;
      }
      open = candidate;
      contentStart = newline + 1;
    }
    if (newline == kNoOffset) {
      break;
    }
    lineStart = newline + 1;
  }
  if (!open || open->indent != 0) {
    return;
  }

  // Confirm that md4c agrees: same language and the content projects to the
  // block text exactly.
  std::string_view info = open->info;
  const size_t infoStart = info.find_first_not_of(" \t\r");
  info = infoStart == kNoOffset ? std::string_view() : info.substr(infoStart);
  const std::string_view language = info.substr(0, info.find_first_of(" \t\r"));
  if (language.find_first_of("\\&") != kNoOffset || language != last.language) {
    return;
  }
  const std::string_view content = source.substr(contentStart);
  if (content.find_first_of(kVerbatimBreakers) != kNoOffset) {
    return;
  }
  std::u16string projected = utf8ToUtf16(content);
  const bool pendingNewline = !projected.empty() && projected.back() == u'\n';
  if (pendingNewline) {
    projected.pop_back();
  }
  if (projected != last.text) {
    return;
  }
  // The line scan above ignores containers and HTML blocks. Probe md4c with
  // one more character from the same proven start: an open fence takes it
  // as code, where a closed block would start a paragraph.
  std::string probe(source.substr(from));
  probe.push_back('x');
  counters_.bytesParsed += probe.size();
  const ParseResult probed = parseMarkdownAt(probe, from, parse_);
  if (pendingNewline) {
    projected.push_back(u'\n');
  }
  projected.push_back(u'x');
  if (probed.status != ParseStatus::Ok || probed.document->blocks.size() != document_->blocks.size() - blocksBefore ||
      probed.document->blocks.back().kind != last.kind || probed.document->blocks.back().text != projected) {
    return;
  }
  openFence_ = true;
  fencePendingNewline_ = pendingNewline;
}

bool IncrementalParser::appendOpenFence() {
  if (!openFence_ || document_->blocks.tail().empty()) {
    return false;
  }
  const std::string_view source = source_;
  const std::string_view added = source.substr(parsedBytes_);
  if (added.find_first_of(kVerbatimBreakers) != kNoOffset) {
    return false;
  }
  // Every line the new bytes touch, including the one they extend, must be
  // unable to close a fence of either kind.
  const size_t previousNewline = source.substr(0, parsedBytes_).rfind('\n');
  size_t lineStart = previousNewline == kNoOffset ? 0 : previousNewline + 1;
  for (;;) {
    const size_t newline = source.find('\n', lineStart);
    const size_t lineEnd = newline == kNoOffset ? source.size() : newline;
    const std::string_view line = source.substr(lineStart, lineEnd - lineStart);
    if (startsWith(line, '`') || startsWith(line, '~')) {
      return false;
    }
    // A `$$` line closes a math fence, so the fast path must decline whenever
    // the new bytes could be one. Otherwise a formula would keep growing past
    // the line that ended it.
    if (parse_.enabled() && (isMathFenceLine(line) || startsWith(line, '$'))) {
      return false;
    }
    if (newline == kNoOffset) {
      break;
    }
    lineStart = newline + 1;
  }

  // Copies only the active tail; committed blocks stay shared.
  auto next = std::make_shared<Document>(*document_);
  next->id = allocateDocumentId();
  next->settledBlocks = static_cast<uint32_t>(std::min<size_t>(settledCount(), UINT32_MAX));
  next->streaming = !finished_;
  std::u16string &text = next->blocks.tail().back().text;
  if (fencePendingNewline_) {
    text.push_back(u'\n');
  }
  appendUtf8(text, added);
  // md4c drops the final line break of code text.
  fencePendingNewline_ = !text.empty() && text.back() == u'\n';
  if (fencePendingNewline_) {
    text.pop_back();
  }
  Block &block = next->blocks.tail().back();
  if (block.kind == BlockKind::CodeBlock && !block.language.empty()) {
    HighlightStats stats;
    lexillaHighlighter().tokenizeAppend(block.language, block.text, &highlight_, &block.tokens,
                                       kHighlightBudgetMs, document_->id, document_->blocks.back().text.size(), next->id);
    block.highlightComplete = highlight_.lastStop != HighlightStop::Budget;
    stats.blocks = 1;
    stats.resumes = highlight_.resumed ? 1 : 0;
    stats.stops[static_cast<size_t>(highlight_.lastStop)] = 1;
    noteHighlight(stats);
  } else {
    highlightTail(next.get());
  }
  layoutTail(next.get());
  publish(next, next->settledBlocks);
  counters_.openFenceFastUpdates++;
  return true;
}

ParseResult IncrementalParser::parse(bool finished) {
  counters_.updates++;
  finished_ = finished;
  if (source_.size() > kMaxSourceBytes) {
    return fullParse(ParseFallback::InvalidSource, false, false);
  }
  scanReferenceDependencies();
  // Full mode takes this branch for a finished stream too, so it has to pass
  // the flag itself: the settled document is never previewed.
  if (mode_ == ParseMode::FullParse) {
    return fullParse(ParseFallback::Count, !finished, finished);
  }
  if (replaced_) {
    return fullParse(ParseFallback::NonAppend, !finished, finished);
  }
  if (finished) {
    return fullParse(ParseFallback::Finished, false, true);
  }
  ParseResult current;
  // A pending policy change has to be applied to what the reader is looking at,
  // so it skips the "nothing new arrived" shortcut below and reparses the tail.
  const bool repaint = previewDirty_;
  previewDirty_ = false;
  if (!repaint && document_ && (source_.size() == parsedBytes_ || appendOpenFence())) {
    current.document = document_;
    return current;
  }

  tryCommit();
  if (source_.size() - committedBytes_ > kMaxRetainedTailBytes) {
    return fullParse(ParseFallback::TailLimit, true, false);
  }
  if (committedBytes_ == 0) {
    return fullParse(ParseFallback::InitialOrUnprovenBoundary, true, false);
  }
  return parseTail();
}

} // namespace downstream
