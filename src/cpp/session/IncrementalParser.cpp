#include "IncrementalParser.h"

#include "CodeBlocks.h"
#include "Compare.h"
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
  if (block.kind == BlockKind::CodeBlock || block.kind == BlockKind::HtmlBlock) {
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

/// Checks that md4c has closed everything the region opened, so parsing any
/// later input cannot change the region's blocks.
bool provesClosed(std::string_view region, const ParseResult &parsed) {
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
    if (hasUnresolvedBracket(block)) {
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
    case BlockKind::Paragraph:
    case BlockKind::TableRow:
    case BlockKind::HtmlBlock:
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

void offsetIds(Block &block, uint32_t containers, uint32_t tables, size_t links) {
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
}

size_t IncrementalParser::committedCount() const {
  return committedBlocks_ ? committedBlocks_->size() : 0;
}

void IncrementalParser::clearCommitted() {
  committedBlocks_.reset();
  committedLinks_.reset();
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

const DocumentHandle &IncrementalParser::publish(DocumentHandle document, uint32_t settled) {
  // The two flags a snapshot carries that its blocks do not: how many leading
  // blocks no later input can change, and whether a stream is still arriving.
  // The second is how an `isAnimating` prop reaches the native rendering. A pass
  // that built its document sets both in place; only a document shared with an
  // earlier snapshot is copied here.
  const bool streaming = !finished_;
  if (document && (document->settledBlocks != settled || document->streaming != streaming)) {
    auto copy = std::make_shared<Document>(*document);
    copy->settledBlocks = settled;
    copy->streaming = streaming;
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
    const remend::PreviewResult preview = preview_.apply(source, 0, committedCount());
    notePreview(preview);
    result = preview.parsed;
  } else {
    result = parseMarkdown(source);
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
  const Document &parsed = *result.document;
  bool confirmed = committed > 0 && source.size() >= committedBytes_ && parsed.blocks.size() >= committed &&
      parsed.links.size() >= committedLinks;
  for (size_t i = 0; confirmed && i < committed; i++) {
    confirmed = sameBlock((*committedBlocks_)[i], parsed.blocks[i]);
  }
  for (size_t i = 0; confirmed && i < committedLinks; i++) {
    const Link &link = (*committedLinks_)[i];
    confirmed = link.url == parsed.links[i].url && link.range == parsed.links[i].range;
  }
  if (confirmed) {
    const auto &blocks = parsed.blocks.tail();
    const auto &links = parsed.links.tail();
    auto shared = std::make_shared<Document>();
    shared->id = parsed.id;
    shared->blocks = SharedList<Block>(committedBlocks_, std::vector<Block>(blocks.begin() + committed, blocks.end()));
    shared->links = SharedList<Link>(committedLinks_, std::vector<Link>(links.begin() + committedLinks, links.end()));
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
  const remend::PreviewResult preview = preview_.apply(tail, committedBytes_);
  notePreview(preview);
  ParseResult result = preview.parsed;
  counters_.tailParses++;
  if (result.status != ParseStatus::Ok) {
    return result;
  }

  const size_t committed = committedCount();
  const size_t committedLinks = committedLinks_ ? committedLinks_->size() : 0;
  std::vector<Block> blocks(result.document->blocks.begin(), result.document->blocks.end());
  for (Block &block : blocks) {
    offsetIds(block, committedContainers_, committedTables_, committedLinks);
  }
  // The tail parse saw an empty document before its first block.
  if (committed > 0 && !blocks.empty()) {
    blocks.front().spacingBefore = BlockSpacing::Normal;
  }
  std::vector<Link> links(result.document->links.begin(), result.document->links.end());
  for (Link &link : links) {
    offsetLink(link, committedUtf16_);
  }
  auto combined = std::make_shared<Document>();
  combined->id = result.document->id;
  combined->blocks = SharedList<Block>(committedBlocks_, std::move(blocks));
  combined->links = SharedList<Link>(committedLinks_, std::move(links));
  // The committed prefix is exactly what md4c has proved no later input can
  // change, so it is what a control or a selection may act on.
  combined->settledBlocks = static_cast<uint32_t>(std::min<size_t>(committed, UINT32_MAX));
  combined->streaming = !finished_;
  counters_.cacheHits += committed;
  result.document = combined;
  highlightTail(combined.get());
  result.document = publish(combined, combined->settledBlocks);
  watchOpenFence(source, committedBytes_, committed);
  return result;
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
  const ParseResult parsed = parseMarkdownAt(region, committedBytes_);
  bool closed = provesClosed(region, parsed) &&
      committedCount() + parsed.document->blocks.size() <= kMaxCommittedBlocks;
  if (closed && !parsed.document->blocks.empty()) {
    const BlockKind kind = parsed.document->blocks.back().kind;
    // Open fences and some HTML blocks run to the end of input.
    if (kind == BlockKind::CodeBlock || kind == BlockKind::HtmlBlock) {
      std::string probe(region);
      probe += "x\n";
      counters_.bytesParsed += probe.size();
      closed = splitsBefore(parsed, parseMarkdownAt(probe, committedBytes_));
    }
  }
  if (!closed) {
    counters_.rejectedCommits++;
    return false;
  }

  const auto &parsedBlocks = parsed.document->blocks;
  const size_t committed = committedCount();
  const size_t committedLinks = committedLinks_ ? committedLinks_->size() : 0;
  Utf16OffsetMap offsets(region);
  // Reused across the region's blocks, so consecutive ones of the same
  // language share a configured lexer.
  HighlightState committedHighlight;
  HighlightStats stats;
  std::vector<Block> blocks;
  blocks.reserve(parsedBlocks.size());
  for (size_t i = 0; i < parsedBlocks.size(); i++) {
    Block block = parsedBlocks[i];
    block.identity = nextBlockIdentity.fetch_add(1, std::memory_order_relaxed);
    const ByteRange bytes = parsed.blockSource[i];
    block.sourceRange = {committedUtf16_ + offsets.toUtf16(bytes.start), committedUtf16_ + offsets.toUtf16(bytes.end)};
    offsetIds(block, committedContainers_, committedTables_, committedLinks);
    if (i == 0 && committed > 0) {
      block.spacingBefore = BlockSpacing::Normal;
    }
    // A committed code block is closed, so it is colored here from its whole
    // text and then shared, unchanged, by every later snapshot. Nothing will
    // ever revisit it, so it is settled rather than streaming: a pass that ran
    // out of budget would otherwise leave the rest of the block plain forever.
    // A local state keeps this away from the tail's.
    if (block.kind == BlockKind::CodeBlock && !block.language.empty()) {
      highlightSettledCodeBlock(lexillaHighlighter(), &block, &committedHighlight, kHighlightBudgetMs, &stats);
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
  noteHighlight(stats);
  committedBytes_ = boundary;
  committedUtf16_ += offsets.toUtf16(region.size());
  committedContainers_ += parsed.containerCount;
  committedTables_ += parsed.tableCount;
  nextBlankAttempt_ = 0;
  nextLineAttempt_ = 0;
  counters_.committedBlocks += parsedBlocks.size();
  return true;
}
void IncrementalParser::watchOpenFence(std::string_view source, size_t from, size_t blocksBefore) {
  openFence_ = false;
  if (mode_ != ParseMode::IncrementalTail || !document_ || document_->blocks.empty()) {
    return;
  }
  const Block &last = document_->blocks.back();
  if (last.kind != BlockKind::CodeBlock || !last.containers.empty() || !last.runs.empty()) {
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
    } else if (fenceOpener(line)) {
      // An opener still being typed can change its info string.
      if (newline == kNoOffset) {
        return;
      }
      open = fenceOpener(line);
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
  const ParseResult probed = parseMarkdownAt(probe, from);
  if (pendingNewline) {
    projected.push_back(u'\n');
  }
  projected.push_back(u'x');
  if (probed.status != ParseStatus::Ok || probed.document->blocks.size() != document_->blocks.size() - blocksBefore ||
      probed.document->blocks.back().kind != BlockKind::CodeBlock || probed.document->blocks.back().text != projected) {
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
    if (newline == kNoOffset) {
      break;
    }
    lineStart = newline + 1;
  }

  // Copies only the active tail; committed blocks stay shared.
  auto next = std::make_shared<Document>(*document_);
  next->id = allocateDocumentId();
  next->settledBlocks = static_cast<uint32_t>(std::min<size_t>(committedCount(), UINT32_MAX));
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
  highlightTail(next.get());
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
