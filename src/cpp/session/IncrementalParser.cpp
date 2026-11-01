#include "IncrementalParser.h"

#include "CodeBlocks.h"
#include "Compare.h"
#include "MathBlocks.h"
#include "RenderReuse.h"
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
constexpr size_t kMaxDefinitionReplayBytes = 8 * 1024;
// Smaller regions reparse cheaply as tail, so a commit attempt costs more
// than it saves.
constexpr size_t kMinCommitBytes = 128;
// md4c reads at most nine digits of an ordered list marker.
constexpr uint32_t kMaxListNumber = 999999999;
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
  // MarkdownParser::recordRun appends disjoint runs in text order, merging
  // adjacent equal styles. Walk that order once instead of searching every
  // run again for each bracket in an inline-code-heavy paragraph.
  size_t runIndex = 0;
  for (size_t index = 0; index < block.text.size(); index++) {
    if (block.text[index] != u'[' && block.text[index] != u']') {
      continue;
    }
    while (runIndex < block.runs.size() && block.runs[runIndex].end <= index) {
      runIndex++;
    }
    const bool inCode = runIndex < block.runs.size() && block.runs[runIndex].start <= index &&
        (block.runs[runIndex].styles & InlineStyle::Code) != 0;
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
  // Only md4c-confirmed link definitions may leave this region. Literal
  // markers and footnotes keep the conservative path. A blank line is required
  // after a definition so a later line cannot extend its destination or title.
  if (!parsed.referenceDefinitions.empty() && !isBlankLine(lastLine(region))) return false;
  for (size_t marker = region.find("]:"); marker != kNoOffset; marker = region.find("]:", marker + 2)) {
    const bool defined = std::any_of(parsed.referenceDefinitions.begin(), parsed.referenceDefinitions.end(),
                                    [marker](const ByteRange &range) {
                                      return range.start <= marker && marker + 2 <= range.end;
                                    });
    if (!defined) return false;
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

void offsetIds(Block &block, uint32_t containers, uint32_t tables, size_t links, size_t images, size_t tags) {
  for (Container &container : block.containers) {
    container.id += containers;
    if (container.tag >= 0) container.tag += static_cast<int32_t>(tags);
  }
  if (block.table != 0) {
    block.table += tables;
  }
  for (InlineRun &run : block.runs) {
    if (run.tag >= 0) run.tag += static_cast<int32_t>(tags);
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

void offsetLink(Link &link, uint32_t utf16, size_t tags) {
  if (link.tag >= 0) link.tag += static_cast<int32_t>(tags);
  // An empty range is unknown in a full parse too; keep it unknown.
  if (link.range.end > link.range.start) {
    link.range.start += utf16;
    link.range.end += utf16;
  }
}

void offsetTag(Tag &tag, uint32_t utf16, size_t bytes) {
  // Anchor metadata has no source range; custom tags always have a known
  // opener, including a local offset of zero. A zero end is an open sentinel.
  if (tag.block || tag.name != "a" || tag.range.start != 0 || tag.range.end != 0) {
    tag.range.start += utf16;
    if (tag.range.end != 0) tag.range.end += utf16;
  }
  if (tag.block) {
    tag.bodyStart += static_cast<uint32_t>(bytes);
    if (tag.bodyEnd != 0) tag.bodyEnd += static_cast<uint32_t>(bytes);
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
  size_t settled = committedCount();
  for (const auto *prefix : {&referencePrefix_, &listPrefix_}) {
    if (*prefix) settled = std::min(settled, (*prefix)->blocks ? (*prefix)->blocks->size() : size_t{0});
  }
  return settled;
}

IncrementalParser::Checkpoint IncrementalParser::checkpoint() const {
  return Checkpoint{committedBlocks_,     committedLinks_,       committedImages_,
                    committedTags_,       committedBytes_,       committedUtf16_,
                    committedContainers_, committedTables_,      committedDefinitions_,
                    committedReferenceOutput_, listContext_};
}

void IncrementalParser::invalidateReferencePrefix() {
  if (!referencePrefix_) return;
  Checkpoint checkpoint = std::move(*referencePrefix_);
  referencePrefix_.reset();
  // A list checkpoint taken at or after this one describes commits that are
  // being revoked with it.
  if (listPrefix_ && listPrefix_->bytes >= checkpoint.bytes) listPrefix_.reset();
  restore(std::move(checkpoint));
}

void IncrementalParser::invalidateListPrefix() {
  if (!listPrefix_) return;
  Checkpoint checkpoint = std::move(*listPrefix_);
  listPrefix_.reset();
  if (referencePrefix_ && referencePrefix_->bytes >= checkpoint.bytes) referencePrefix_.reset();
  restore(std::move(checkpoint));
}

void IncrementalParser::restore(Checkpoint checkpoint) {
  committedBlocks_ = checkpoint.blocks;
  committedLinks_ = checkpoint.links;
  committedImages_ = checkpoint.images;
  committedTags_ = checkpoint.tags;
  committedBytes_ = checkpoint.bytes;
  committedUtf16_ = checkpoint.utf16;
  committedContainers_ = checkpoint.containers;
  committedTables_ = checkpoint.tables;
  committedDefinitions_ = checkpoint.definitions;
  committedReferenceOutput_ = checkpoint.referenceOutputUsed;
  listContext_ = checkpoint.list;
  plainParagraph_ = false;
  openFence_ = false;
  highlight_.clear();
  resetScan();
}

void IncrementalParser::scanReferenceDependencies() {
  // Scan only new bytes, including overlap for a split marker. Every newly
  // arriving definition marker revokes unresolved blocks. Existing confirmed
  // link definitions do not permanently prevent later prose from being shared.
  const size_t from = dependencyScannedBytes_ > 0 ? dependencyScannedBytes_ - 1 : 0;
  const std::string_view added = std::string_view(source_).substr(from);
  const bool marker = added.find("]:") != kNoOffset;
  referenceDependentSource_ = referenceDependentSource_ || added.find("[^") != kNoOffset ||
      (parse_.enabled() && parse_.brackets() &&
       (added.find("\\[") != kNoOffset || added.find("\\(") != kNoOffset));
  dependencyScannedBytes_ = source_.size();
  if (marker || referenceDependentSource_ || preview_.enabled() || previousReferenceLimited_)
    invalidateReferencePrefix();
  // Footnotes and bracket math keep the full-document proof, which a tail
  // parsed after a list lead does not have.
  if (referenceDependentSource_ || preview_.enabled()) invalidateListPrefix();
}

void IncrementalParser::clearCommitted() {
  referencePrefix_.reset();
  listPrefix_.reset();
  listContext_.reset();
  committedBlocks_.reset();
  committedLinks_.reset();
  committedImages_.reset();
  committedTags_.reset();
  committedBytes_ = 0;
  committedUtf16_ = 0;
  committedContainers_ = 0;
  committedTables_ = 0;
  committedDefinitions_.clear();
  committedReferenceOutput_ = 0;
  previousReferenceLimited_ = false;
  plainParagraph_ = false;
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
  const uint64_t renderReuse = finished_ && document_ && document_->streaming && document &&
      document_->id != document->id && canReuseRendering(*document_, *document) ? document_->id : 0;
  if (document && (document->settledBlocks != settled || document->streaming != streaming || hideTables ||
                   document->renderReuseFrom != renderReuse)) {
    auto copy = std::make_shared<Document>(*document);
    copy->settledBlocks = settled;
    copy->streaming = streaming;
    copy->renderReuseFrom = renderReuse;
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
  previousReferenceLimited_ = result.referenceOutputLimited;
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
  if (parse_.codeHighlighting()) {
    result.document = highlightSettledCodeBlocks(downstreamHighlighter(), result.document, kHighlightBudgetMs, &stats,
                                                 replaced_ ? nullptr : document_);
  }
  noteHighlight(stats);
  // Math runs after highlighting and before anything compares blocks, for the
  // same reason: a layout is part of what a block draws, so comparing a block
  // whose formula has not been laid out against one whose formula has would
  // find them different. A MathSpan's own identity excludes its layout, so a
  // block keeps its identity while the pass fills it in.
  if (settled) {
    result.document = withSettledBlocks(result.document,
        static_cast<uint32_t>(std::min<size_t>(result.document->blocks.size(), UINT32_MAX)));
  }
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

  // Global formula overflow changes already committed rendering too. Its
  // canonical full parse must own those blocks instead of sharing old layouts.
  if (settled && countFormulas(*result.document) > math_limits::kMaxFormulas) clearCommitted();

  // A fallback reparses everything, but committed blocks keep their shared
  // storage and identity when md4c still produces them unchanged.
  const size_t committed = committedCount();
  const size_t committedLinks = committedLinks_ ? committedLinks_->size() : 0;
  const size_t committedImages = committedImages_ ? committedImages_->size() : 0;
  const size_t committedTags = committedTags_ ? committedTags_->size() : 0;
  const Document &parsed = *result.document;
  bool confirmed = committed > 0 && source.size() >= committedBytes_ && parsed.blocks.size() >= committed &&
      parsed.links.size() >= committedLinks && parsed.images.size() >= committedImages &&
      parsed.tags.size() >= committedTags;
  for (size_t i = 0; confirmed && i < committed; i++) {
    confirmed = document_ && sameRenderBlock(*document_, (*committedBlocks_)[i], parsed, parsed.blocks[i]);
  }
  for (size_t i = 0; confirmed && i < committedLinks; i++) {
    const Link &link = (*committedLinks_)[i];
    confirmed = link.url == parsed.links[i].url && link.range == parsed.links[i].range &&
        link.tag == parsed.links[i].tag;
  }
  for (size_t i = 0; confirmed && i < committedImages; i++) {
    confirmed = scan::sameImage((*committedImages_)[i], parsed.images[i]);
  }
  for (size_t i = 0; confirmed && i < committedTags; i++) {
    confirmed = scan::sameTag((*committedTags_)[i], parsed.tags[i]);
  }
  if (confirmed) {
    const auto &blocks = parsed.blocks.tail();
    const auto &links = parsed.links.tail();
    const auto &images = parsed.images.tail();
    const auto &tags = parsed.tags.tail();
    auto shared = std::make_shared<Document>();
    shared->id = parsed.id;
    shared->blocks = SharedList<Block>(committedBlocks_, std::vector<Block>(blocks.begin() + committed, blocks.end()));
    shared->links = SharedList<Link>(committedLinks_, std::vector<Link>(links.begin() + committedLinks, links.end()));
    shared->images = SharedList<Image>(committedImages_, std::vector<Image>(images.begin() + committedImages, images.end()));
    shared->tags = SharedList<Tag>(committedTags_, std::vector<Tag>(tags.begin() + committedTags, tags.end()));
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
  if (!finished_) {
    watchOpenFence(source, committedBytes_, committedCount());
    watchPlainParagraph();
  } else {
    openFence_ = false;
    plainParagraph_ = false;
  }
  return result;
}

void IncrementalParser::noteHighlight(const HighlightStats &stats) {
  counters_.blocksHighlighted += stats.blocks;
  counters_.highlightResumes += stats.resumes;
  counters_.highlightBytesLexed += stats.bytesLexed;
  counters_.highlightRunsBuilt += stats.runsBuilt;
  counters_.highlightTokenBytesCopied += stats.tokenBytesCopied;
  counters_.highlightTextBytesCopied += stats.textBytesCopied;
  for (size_t index = 0; index < kHighlightStopCount; index++) {
    counters_.highlight[index] += stats.stops[index];
  }
}

void IncrementalParser::highlightTail(Document *document) {
  if (!parse_.codeHighlighting()) return;
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
    if (i + 1 == last && absolute + 1 == document->blocks.size() && !finished_) {
      highlightCodeBlock(downstreamHighlighter(), &block, &highlight_, kHighlightBudgetMs, &stats);
    } else {
      // Earlier blocks cannot grow. A budget cut must be finished now because
      // no later snapshot can otherwise complete their highlighting.
      highlightSettledCodeBlock(downstreamHighlighter(), &block, i + 1 == last ? &highlight_ : &settled,
                                kHighlightBudgetMs, &stats);
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
  if (finished_) {
    for (Block &block : document->blocks.tail()) layoutMathInBlock(&block, &stats);
  } else {
    layoutMathTail(&document->blocks.tail(), document_.get(), &stats);
  }
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
  while (stable < limit && sameRenderBlock(*previous, previous->blocks[stable], *result.document, blocks[stable])) {
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
  ParseResult result;
  if (finished_) {
    // The committed prefix is canonical. Finish only its raw tail, without
    // provisional repairs or dependency replay changing the source grammar.
    result = parseMarkdownAt(tail, committedBytes_, parse_);
    counters_.bytesParsed += tail.size();
  } else if (committedDefinitions_.empty() && !listContext_) {
    const remend::PreviewResult preview = preview_.apply(tail, committedBytes_, 0, parse_);
    notePreview(preview);
    result = preview.parsed;
  } else {
    // Preview and bracket math can rewrite the synthetic prefix. Keep their
    // full-document proof until they have an explicit definition-context API.
    if (preview_.enabled() || referenceDependentSource_) {
      return fullParse(ParseFallback::InitialOrUnprovenBoundary, true, false);
    }
    result = parseAfterCommitted(tail);
  }
  counters_.tailParses++;
  if (listContext_ && (result.status != ParseStatus::Ok || !keepsListContext(result))) {
    // The list turned loose, or the tail no longer continues it, so the
    // provisional items go back to the tail and are parsed with it.
    invalidateListPrefix();
    return committedBytes_ == 0 ? fullParse(ParseFallback::InitialOrUnprovenBoundary, true, false) : parseTail();
  }
  if (result.status != ParseStatus::Ok) {
    return result;
  }

  const size_t committed = committedCount();
  const size_t committedLinks = committedLinks_ ? committedLinks_->size() : 0;
  const size_t committedImages = committedImages_ ? committedImages_->size() : 0;
  const size_t committedTags = committedTags_ ? committedTags_->size() : 0;
  previousReferenceLimited_ = result.referenceOutputLimited;
  std::vector<Block> blocks(result.document->blocks.begin(), result.document->blocks.end());
  for (Block &block : blocks) {
    offsetIds(block, committedContainers_, committedTables_, committedLinks, committedImages, committedTags);
  }
  // The tail parse saw an empty document before its first block, unless it
  // saw the list lead, which spaced it as the list's next item.
  if (committed > 0 && !blocks.empty() && !listContext_) {
    blocks.front().spacingBefore = BlockSpacing::Normal;
  }
  std::vector<Link> links(result.document->links.begin(), result.document->links.end());
  for (Link &link : links) {
    offsetLink(link, committedUtf16_, committedTags);
  }
  std::vector<Image> images(result.document->images.begin(), result.document->images.end());
  for (Image &image : images) {
    offsetImage(image, static_cast<int32_t>(committedLinks), committedUtf16_);
  }
  std::vector<Tag> tags(result.document->tags.begin(), result.document->tags.end());
  for (Tag &tag : tags) offsetTag(tag, committedUtf16_, committedBytes_);
  auto combined = std::make_shared<Document>();
  combined->id = result.document->id;
  combined->blocks = SharedList<Block>(committedBlocks_, std::move(blocks));
  combined->links = SharedList<Link>(committedLinks_, std::move(links));
  combined->images = SharedList<Image>(committedImages_, std::move(images));
  combined->tags = SharedList<Tag>(committedTags_, std::move(tags));
  // Only the strict prefix is settled. Shared provisional reference blocks
  // remain mutable and are never offered as settled text to controls.
  combined->settledBlocks = static_cast<uint32_t>(std::min<size_t>(
      finished_ ? combined->blocks.size() : settledCount(), UINT32_MAX));
  combined->streaming = !finished_;
  // The formula cap is global: an over-cap final document must clear layouts
  // in the prefix too, so shared-prefix finalization cannot handle that case.
  if (finished_ && countFormulas(*combined) > math_limits::kMaxFormulas) {
    return fullParse(ParseFallback::Finished, false, true);
  }
  counters_.cacheHits += committed;
  result.document = combined;
  highlightTail(combined.get());
  layoutTail(combined.get());
  result.document = publish(combined, combined->settledBlocks);
  if (!finished_) {
    watchOpenFence(source, committedBytes_, committed);
    watchPlainParagraph();
  } else {
    openFence_ = false;
    plainParagraph_ = false;
    highlight_.clear();
  }
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
  std::string_view region = std::string_view(source_).substr(committedBytes_, boundary - committedBytes_);
  counters_.commitAttempts++;
  ParseResult parsed = parseAfterCommitted(region);
  if (listContext_ && parsed.status == ParseStatus::Ok && !keepsListContext(parsed)) {
    invalidateListPrefix();
    counters_.rejectedCommits++;
    return false;
  }
  const bool allowReferences = !referenceDependentSource_ && !preview_.enabled() &&
      !parsed.referenceOutputLimited &&
      std::string_view(source_).substr(boundary).find("]:") == kNoOffset;
  bool closed = (!referenceDependentSource_ || parsed.referenceDefinitions.empty()) &&
      (!preview_.enabled() || parsed.referenceDefinitions.empty()) &&
      provesClosed(region, parsed, allowReferences) &&
      committedCount() + parsed.document->blocks.size() <= kMaxCommittedBlocks;
  if (closed && !parsed.document->blocks.empty()) {
    const BlockKind kind = parsed.document->blocks.back().kind;
    // Open fences and some HTML blocks run to the end of input. A `$$` line
    // opens a formula as well as closing one, so a math block is probed too.
    if (kind == BlockKind::CodeBlock || kind == BlockKind::HtmlBlock || kind == BlockKind::MathBlock) {
      std::string probe(region);
      probe += "x\n";
      closed = splitsBefore(parsed, parseAfterCommitted(probe));
    }
  }
  std::string definitions = committedDefinitions_;
  if (closed && !parsed.referenceDefinitions.empty()) {
    for (const ByteRange &range : parsed.referenceDefinitions) {
      definitions.append(region.substr(range.start, range.end - range.start));
      definitions += "\n\n";
    }
    if (definitions.size() > kMaxDefinitionReplayBytes) {
      closed = false;
    } else {
      // Replaying original ranges is supported only when md4c confirms they
      // contain definitions alone. Container prefixes and other unsupported
      // spellings therefore cannot silently change the tail grammar.
      counters_.bytesParsed += definitions.size();
      const auto replay = parseMarkdown(definitions, parse_);
      closed = replay.status == ParseStatus::Ok && replay.document->blocks.empty() &&
          replay.containerCount == 0 && replay.referenceDefinitions.size() >= parsed.referenceDefinitions.size();
    }
  }
  // A region that ends inside a top-level list commits the items before its
  // last one: a sibling item starting closes every block before it. Only the
  // list's tightness can still change, which is why these commits stay
  // provisional until a commit ends after the list.
  std::optional<ListContext> nextList;
  if (!closed && !preview_.enabled() && !referenceDependentSource_ && parsed.status == ParseStatus::Ok &&
      parsed.referenceDefinitions.empty() && !parsed.referenceOutputLimited) {
    const auto [count, end] = finishedListItems(region, parsed);
    // finishedListItems finds items only in a region that ends inside a list.
    const ListSummary list = count > 0 ? parsed.lists.back() : ListSummary{};
    // The number before the tail's first item, which the lead takes.
    const uint32_t number = list.ordered ? list.nextNumber - 2 : 0;
    if (count > 0 && number <= kMaxListNumber && committedCount() + count <= kMaxCommittedBlocks) {
      const std::string_view items = region.substr(0, end);
      ParseResult head = parseAfterCommitted(items);
      bool confirmed = head.status == ParseStatus::Ok && head.document->blocks.size() == count &&
          head.referenceDefinitions.empty() && !head.referenceOutputLimited &&
          (!listContext_ || keepsListContext(head));
      // An empty item left open at the end can still take the next line.
      uint32_t lastContainer = 0;
      for (size_t i = 0; confirmed && i < count; i++) {
        confirmed = sameBlock(head.document->blocks[i], parsed.document->blocks[i]);
        for (const Container &container : head.document->blocks[i].containers) {
          lastContainer = std::max(lastContainer, container.id);
        }
      }
      // The definition markers that matter start after the shorter region.
      const bool itemsAllowReferences =
          allowReferences && std::string_view(source_).substr(committedBytes_ + end).find("]:") == kNoOffset;
      confirmed = confirmed && lastContainer == head.containerCount &&
          (itemsAllowReferences ||
           std::none_of(head.document->blocks.begin(), head.document->blocks.end(), hasUnresolvedBracket));
      if (confirmed) {
        parsed = std::move(head);
        region = items;
        boundary = committedBytes_ + end;
        nextList = ListContext{list.ordered, list.mark, number, list.tight};
        closed = true;
      }
    }
  }
  if (!closed) {
    counters_.rejectedCommits++;
    return false;
  }

  if (nextList && !listPrefix_) {
    listPrefix_ = checkpoint();
  }
  const auto &parsedBlocks = parsed.document->blocks;
  if (!referencePrefix_ && std::any_of(parsedBlocks.begin(), parsedBlocks.end(), hasUnresolvedBracket)) {
    referencePrefix_ = checkpoint();
  }
  const size_t committed = committedCount();
  const size_t committedLinks = committedLinks_ ? committedLinks_->size() : 0;
  const size_t committedImages = committedImages_ ? committedImages_->size() : 0;
  const size_t committedTags = committedTags_ ? committedTags_->size() : 0;
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
    // A provisional block gets an identity too. Renderers reuse layout by
    // identity, so without one every block after an unresolved reference is
    // rebuilt on every snapshot. Revoking drops these blocks, and their
    // replacements take fresh identities, so an identity still names one
    // content; settledBlocks alone says which blocks can no longer change.
    block.identity = nextBlockIdentity.fetch_add(1, std::memory_order_relaxed);
    const ByteRange bytes = parsed.blockSource[i];
    block.sourceRange = {committedUtf16_ + offsets.toUtf16(bytes.start), committedUtf16_ + offsets.toUtf16(bytes.end)};
    offsetIds(block, committedContainers_, committedTables_, committedLinks, committedImages, committedTags);
    if (i == 0 && committed > 0 && !listContext_) {
      block.spacingBefore = BlockSpacing::Normal;
    }
    // A closed code block is colored here from its whole text before sharing.
    // The provisional prefix can be revoked, but while it is shared no later
    // pass revisits its tokens. Complete them now so a budget stop does not
    // leave the rest of the shared block plain forever.
    // A local state keeps this away from the tail's.
    if (parse_.codeHighlighting() && block.kind == BlockKind::CodeBlock && !block.language.empty()) {
      highlightSettledCodeBlock(downstreamHighlighter(), &block, &committedHighlight, kHighlightBudgetMs, &stats);
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
      offsetLink(link, committedUtf16_, committedTags);
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
  if (!parsed.document->tags.empty()) {
    std::vector<Tag> tags(parsed.document->tags.begin(), parsed.document->tags.end());
    for (Tag &tag : tags) offsetTag(tag, committedUtf16_, committedBytes_);
    committedTags_ = SharedList<Tag>::Prefix::extend(committedTags_, std::move(tags));
  }
  noteHighlight(stats);
  noteMath(committedMath);
  committedDefinitions_ = std::move(definitions);
  committedReferenceOutput_ += parsed.referenceOutputUsed;
  committedBytes_ = boundary;
  committedUtf16_ += offsets.toUtf16(region.size());
  committedContainers_ += parsed.containerCount;
  committedTables_ += parsed.tableCount;
  nextBlankAttempt_ = 0;
  nextLineAttempt_ = 0;
  if (!referencePrefix_) counters_.committedBlocks += parsedBlocks.size();
  // A commit that ends outside every list closed the one the earlier items
  // belonged to, so their spacing is final.
  listContext_ = nextList;
  if (!nextList) listPrefix_.reset();
  return true;
}

std::pair<size_t, size_t> IncrementalParser::finishedListItems(std::string_view region, const ParseResult &parsed) const {
  const std::pair<size_t, size_t> none{0, 0};
  const auto &blocks = parsed.document->blocks;
  if (parsed.lists.empty() || blocks.empty() || parsed.blockSource.size() != blocks.size()) return none;
  const ListSummary &list = parsed.lists.back();
  if (list.items < 2) return none;
  const auto inLastItem = [&](const Block &block) {
    return !block.containers.empty() && block.containers[0].kind == ContainerKind::ListItem &&
        block.containers[0].id == list.lastItem;
  };
  if (!inLastItem(blocks.back())) return none;
  size_t first = blocks.size() - 1;
  while (first > 0 && inLastItem(blocks[first - 1])) first--;
  if (first == 0 || !blocks[first].containers[0].showsMarker) return none;
  // The line the item's first text is on has to be the line its marker is on.
  // An item whose text starts on a later line is left alone.
  const size_t text = parsed.blockSource[first].start;
  const size_t newline = text == 0 ? kNoOffset : region.rfind('\n', text - 1);
  if (newline == kNoOffset) return none;
  const size_t lineStart = newline + 1;
  size_t at = lineStart;
  while (at < region.size() && at - lineStart < 3 && region[at] == ' ') at++;
  if (list.ordered) {
    const size_t digits = at;
    while (at < region.size() && region[at] >= '0' && region[at] <= '9') at++;
    if (at == digits || at - digits > 9) return none;
  }
  if (at >= region.size() || region[at] != list.mark) return none;
  at++;
  if (at >= region.size() || (region[at] != ' ' && region[at] != '\t') || at > text) return none;
  return {first, lineStart};
}

std::string IncrementalParser::listLead() const {
  if (!listContext_) return {};
  // The lead sits at the tail's own indent, so the tail's first item starts
  // beside it rather than inside it.
  const std::string_view tail = std::string_view(source_).substr(committedBytes_);
  size_t indent = 0;
  while (indent < tail.size() && indent < 3 && tail[indent] == ' ') indent++;
  std::string lead(indent, ' ');
  if (listContext_->ordered) lead += std::to_string(listContext_->number);
  lead += listContext_->mark;
  // A blank line between two items makes a list loose, which is how the lead
  // passes on a looseness the committed items already showed.
  lead += listContext_->tight ? " x\n" : " x\n\n";
  return lead;
}

ParseResult IncrementalParser::parseAfterCommitted(std::string_view region) {
  const uint32_t totalBudget = static_cast<uint32_t>(16 * std::min<size_t>(source_.size(), kMaxSourceBytes / 16));
  const std::string lead = listLead();
  counters_.bytesParsed += region.size() + committedDefinitions_.size() + lead.size();
  return parseMarkdownWithReferences(region, committedBytes_, committedDefinitions_, parse_,
                                     totalBudget - std::min(totalBudget, committedReferenceOutput_), lead);
}

bool IncrementalParser::keepsListContext(const ParseResult &parsed) const {
  return !listContext_ || (!parsed.lists.empty() && parsed.lists.front().tight == listContext_->tight);
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
  fenceLineStart_ = lineStart;
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
  size_t lineStart = fenceLineStart_;
  size_t searchStart = parsedBytes_;
  for (;;) {
    const size_t newline = source.find('\n', searchStart);
    const size_t lineEnd = newline == kNoOffset ? source.size() : newline;
    counters_.fenceAppendScanBytes += lineEnd - searchStart + (newline == kNoOffset ? 0 : 1);
    const std::string_view line = source.substr(lineStart, lineEnd - lineStart);
    if (startsWith(line, '`') || startsWith(line, '~')) {
      return false;
    }
    // A `$$` line closes a math fence, so the fast path must decline whenever
    // the new bytes could be one. Otherwise a formula would keep growing past
    // the line that ended it.
    if (parse_.enabled() && startsWith(line, '$')) {
      return false;
    }
    if (newline == kNoOffset) {
      break;
    }
    lineStart = newline + 1;
    searchStart = lineStart;
  }

  // Copies only the active tail; committed blocks stay shared.
  auto next = std::make_shared<Document>(*document_);
  next->id = allocateDocumentId();
  next->settledBlocks = static_cast<uint32_t>(std::min<size_t>(settledCount(), UINT32_MAX));
  next->streaming = !finished_;
  std::u16string delta;
  if (fencePendingNewline_) {
    delta.push_back(u'\n');
  }
  appendUtf8(delta, added);
  // md4c drops the final line break of code text.
  fencePendingNewline_ = !delta.empty() && delta.back() == u'\n';
  if (fencePendingNewline_) {
    delta.pop_back();
  }
  Block &block = next->blocks.tail().back();
  fenceLineStart_ = lineStart;
  counters_.fenceSnapshotStorageBytesCopied += block.text.appendSnapshot(delta);
  if (parse_.codeHighlighting() && block.kind == BlockKind::CodeBlock && !block.language.empty()) {
    HighlightStats stats;
    downstreamHighlighter().tokenizeAppend(block.language, block.text, delta, &highlight_, &block.tokens,
                                       kHighlightBudgetMs, document_->id, document_->blocks.back().text.size(), next->id, !finished_, fencePendingNewline_);
    block.highlightComplete = highlight_.lastStop != HighlightStop::Budget &&
        highlight_.lastStop != HighlightStop::Deferred;
    block.highlightCanonical = highlight_.canonical || highlight_.lastStop == HighlightStop::Skipped;
    stats.blocks = 1;
    stats.resumes = highlight_.resumed ? 1 : 0;
    stats.bytesLexed = highlight_.bytesLexed;
    stats.runsBuilt = highlight_.runsBuilt;
    stats.tokenBytesCopied = highlight_.tokenBytesCopied;
    stats.textBytesCopied = highlight_.textBytesCopied;
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

namespace {
bool isPlainProse(std::string_view text) {
  return std::all_of(text.begin(), text.end(), [](unsigned char byte) {
    return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
        (byte >= '0' && byte <= '9') || byte == ' ' || byte == '.';
  });
}
bool hasWwwAutolinkCandidate(std::string_view text) {
  for (size_t i = 0; i + 3 < text.size(); i++) {
    if ((text[i] == 'w' || text[i] == 'W') && (text[i + 1] == 'w' || text[i + 1] == 'W') &&
        (text[i + 2] == 'w' || text[i + 2] == 'W') && text[i + 3] == '.') return true;
  }
  return false;
}
} // namespace

void IncrementalParser::watchPlainParagraph() {
  plainParagraph_ = false;
  if (!document_ || preview_.enabled() || document_->blocks.tail().size() != 1 ||
      source_.size() <= committedBytes_) return;
  const Block &block = document_->blocks.tail().back();
  if (block.kind != BlockKind::Paragraph || !block.containers.empty() ||
      !block.math.empty() || !block.images.empty() || block.runs.size() > 1) return;
  if (!block.runs.empty()) {
    const InlineRun &run = block.runs.front();
    if (run.styles != 0 || run.link >= 0 || run.tag >= 0 || run.start != 0 || run.end != block.text.size()) return;
  }
  const std::string_view raw = std::string_view(source_).substr(committedBytes_);
  // Only letters, digits, spaces and literal periods are admitted. Markdown
  // delimiters, entity/HTML openers, URL schemes and mail addresses need other
  // punctuation. WWW links are the remaining exception and are rejected even
  // before they resolve. An alphabetic opener rules out a growing list marker.
  // A newline always reparses for setext/table promotion and hard breaks.
  const bool alphabetic = (raw.front() >= 'a' && raw.front() <= 'z') ||
      (raw.front() >= 'A' && raw.front() <= 'Z');
  if (!alphabetic || !isPlainProse(raw) || hasWwwAutolinkCandidate(raw)) return;
  size_t end = raw.size();
  while (end > 0 && raw[end - 1] == ' ') end--;
  if (block.text != utf8ToUtf16(raw.substr(0, end))) return;
  plainParagraphStart_ = committedBytes_;
  paragraphPendingSpaces_ = raw.size() - end;
  plainParagraph_ = true;
}

bool IncrementalParser::appendPlainParagraph() {
  if (!plainParagraph_ || preview_.enabled() || plainParagraphStart_ != committedBytes_ ||
      document_->blocks.tail().size() != 1) return false;
  const std::string_view added = std::string_view(source_).substr(parsedBytes_);
  const size_t checkStart = std::max(plainParagraphStart_, parsedBytes_ > 3 ? parsedBytes_ - 3 : 0);
  if (!isPlainProse(added) || hasWwwAutolinkCandidate(std::string_view(source_).substr(checkStart))) {
    plainParagraph_ = false;
    return false;
  }
  auto next = std::make_shared<Document>(*document_);
  next->id = allocateDocumentId();
  Block &block = next->blocks.tail().back();
  block.text.append(paragraphPendingSpaces_, u' ');
  block.text.append(added.begin(), added.end());
  size_t end = block.text.size();
  while (end > 0 && block.text[end - 1] == u' ') end--;
  paragraphPendingSpaces_ = block.text.size() - end;
  block.text.resize(end);
  if (!block.runs.empty()) block.runs.front().end = static_cast<uint32_t>(end);
  publish(next, static_cast<uint32_t>(settledCount()));
  counters_.plainParagraphFastUpdates++;
  return true;
}

bool IncrementalParser::canFinishTail() const {
  if (committedBytes_ == 0 || referencePrefix_ || listPrefix_ || listContext_ || !committedDefinitions_.empty() ||
      referenceDependentSource_ || previousReferenceLimited_ || preview_.enabled() || previewDirty_ ||
      source_.find("]:") != std::string::npos) return false;
  if (committedTags_) {
    for (size_t i = 0; i < committedTags_->size(); ++i) {
      const Tag &tag = (*committedTags_)[i];
      if (tag.block ? tag.bodyEnd == 0 : (tag.name != "a" && tag.range.end == 0)) return false;
    }
  }
  if (committedBlocks_) {
    for (size_t i = 0; i < committedBlocks_->size(); ++i) {
      const Block &block = (*committedBlocks_)[i];
      for (const Container &container : block.containers) {
        if (container.open) return false;
      }
    }
  }
  return true;
}

ParseResult IncrementalParser::parse(bool finished) {
  counters_.updates++;
  finished_ = finished;
  if (source_.size() > kMaxSourceBytes) {
    return fullParse(ParseFallback::InvalidSource, false, false);
  }
  scanReferenceDependencies();
  if (finished && document_ && !document_->streaming && !replaced_ && parsedBytes_ == source_.size()) {
    previewDirty_ = false;
    ParseResult current;
    current.document = document_;
    return current;
  }
  // Full mode takes this branch for a finished stream too, so it has to pass
  // the flag itself: the settled document is never previewed.
  if (mode_ == ParseMode::FullParse) {
    return fullParse(ParseFallback::Count, !finished, finished);
  }
  if (replaced_) {
    return fullParse(ParseFallback::NonAppend, !finished, finished);
  }
  if (finished) {
    const bool tail = canFinishTail();
    previewDirty_ = false;
    return tail ? parseTail() : fullParse(ParseFallback::Finished, false, true);
  }
  ParseResult current;
  // A pending policy change has to be applied to what the reader is looking at,
  // so it skips the "nothing new arrived" shortcut below and reparses the tail.
  const bool repaint = previewDirty_;
  previewDirty_ = false;
  if (!repaint && document_ && (source_.size() == parsedBytes_ || appendOpenFence() || appendPlainParagraph())) {
    current.document = document_;
    return current;
  }

  tryCommit();
  if (committedBytes_ == 0 && source_.size() > kMaxRetainedTailBytes) {
    return fullParse(ParseFallback::TailLimit, true, false);
  }
  if (committedBytes_ == 0) {
    return fullParse(ParseFallback::InitialOrUnprovenBoundary, true, false);
  }
  return parseTail();
}

} // namespace downstream
