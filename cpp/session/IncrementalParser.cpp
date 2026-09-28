#include "IncrementalParser.h"

#include "Unicode.h"

#include <algorithm>
#include <iterator>
#include <memory>

namespace downstream {
namespace {

constexpr size_t kMaxRetainedTailBytes = 64 * 1024;
constexpr size_t kMaxCommittedBlocks = 8192;

bool plainByte(unsigned char c) {
  return c >= 0x80 || c == ' ' || (c >= 'a' && c <= 'z') ||
      (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

bool safeFenceContent(std::string_view text) {
  for (unsigned char c : text) {
    if (c != '\n' && c != ' ' && !(c >= 'a' && c <= 'z') &&
        !(c >= 'A' && c <= 'Z') && !(c >= '0' && c <= '9') && c < 0x80) {
      return false;
    }
  }
  return isValidUtf8(text);
}

struct ParagraphRange {
  size_t start;
  size_t end;
};

// The caller has already found a safe boundary. This repeats the inexpensive
// line walk to map the parsed paragraphs back to their exact source spans.
std::vector<ParagraphRange> paragraphRanges(std::string_view source) {
  std::vector<ParagraphRange> ranges;
  size_t paragraphStart = std::string_view::npos;
  size_t paragraphEnd = 0;
  size_t lineStart = 0;
  while (lineStart < source.size()) {
    const size_t newline = source.find('\n', lineStart);
    const size_t lineLimit = newline == std::string_view::npos ? source.size() : newline;
    const size_t lineEnd = lineLimit > lineStart && source[lineLimit - 1] == '\r' ? lineLimit - 1 : lineLimit;
    if (lineEnd == lineStart) {
      if (paragraphStart != std::string_view::npos) {
        ranges.push_back({paragraphStart, paragraphEnd});
        paragraphStart = std::string_view::npos;
      }
    } else {
      if (paragraphStart == std::string_view::npos) {
        paragraphStart = lineStart;
      }
      paragraphEnd = lineEnd;
    }
    lineStart = newline == std::string_view::npos ? source.size() : newline + 1;
  }
  return ranges;
}

} // namespace

void IncrementalParser::reset() {
  source_.clear();
  document_.reset();
  committed_.clear();
  committedBytes_ = 0;
  committedUtf16_ = 0;
  nextBlockId_ = 1;
  openFenceFast_ = false;
  rawCodeText_.clear();
  counters_ = {};
}

ParseResult IncrementalParser::fullParse(std::string_view source, ParseFallback reason) {
  ParseResult result = parseMarkdown(source);
  counters_.fullParses++;
  counters_.bytesParsed += source.size();
  if (reason != ParseFallback::Count) {
    counters_.fallback[static_cast<size_t>(reason)]++;
  }
  if (result.status == ParseStatus::Ok) {
    bool confirmedPrefix = false;
    // A fallback may reparse the whole source, but proven paragraph spans
    // retain their identity if both the source prefix and render model agree.
    if (!committed_.empty() && source.size() >= committedBytes_ &&
        (reason == ParseFallback::TailLimit ||
         source.substr(0, committedBytes_) == std::string_view(source_).substr(0, committedBytes_)) &&
        result.document->blocks.size() >= committed_.size()) {
      bool samePrefix = true;
      for (size_t i = 0; i < committed_.size(); i++) {
        const Block &oldBlock = committed_[i];
        const Block &newBlock = result.document->blocks[i];
        if (oldBlock.kind != newBlock.kind || oldBlock.text != newBlock.text ||
            oldBlock.spacingBefore != newBlock.spacingBefore || !newBlock.containers.empty() ||
            !newBlock.runs.empty()) {
          samePrefix = false;
          break;
        }
      }
      if (samePrefix) {
        confirmedPrefix = true;
        auto identified = std::make_shared<Document>(*result.document);
        for (size_t i = 0; i < committed_.size(); i++) {
          identified->blocks[i].identity = committed_[i].identity;
          identified->blocks[i].sourceRange = committed_[i].sourceRange;
        }
        result.document = identified;
        counters_.cacheHits += committed_.size();
      }
    }
    source_.assign(source);
    document_ = result.document;
    openFenceFast_ = false;
    rawCodeText_.clear();
    if (mode_ == ParseMode::IncrementalTail && source.starts_with("```\n") &&
        source.size() - 4 <= kMaxRetainedTailBytes &&
        result.document->blocks.size() == 1 && result.document->blocks[0].kind == BlockKind::CodeBlock &&
        result.document->blocks[0].language.empty() && safeFenceContent(source.substr(4))) {
      rawCodeText_ = utf8ToUtf16(source.substr(4));
      std::u16string projected = rawCodeText_;
      if (!projected.empty() && projected.back() == u'\n') projected.pop_back();
      openFenceFast_ = projected == result.document->blocks[0].text;
      if (!openFenceFast_) rawCodeText_.clear();
    }
    // Tail-limit fallback can recur on every append. Keep the proven index
    // so the same completed blocks retain their identities across updates.
    if (reason != ParseFallback::TailLimit || !confirmedPrefix) {
      committed_.clear();
      committedBytes_ = 0;
      committedUtf16_ = 0;
    }
  }
  return result;
}

size_t IncrementalParser::findSafeBoundary(std::string_view source) const {
  size_t lastBoundary = committedBytes_;
  size_t lineStart = committedBytes_;
  while (lineStart < source.size()) {
    const size_t newline = source.find('\n', lineStart);
    if (newline == std::string_view::npos) {
      break;
    }
    const size_t lineEnd = newline > lineStart && source[newline - 1] == '\r' ? newline - 1 : newline;
    size_t leadingSpaces = 0;
    for (size_t i = lineStart; i < lineEnd; i++) {
      if (!plainByte(static_cast<unsigned char>(source[i]))) {
        return lastBoundary;
      }
      if (i == lineStart + leadingSpaces && source[i] == ' ') {
        leadingSpaces++;
      }
    }
    // Four leading spaces can turn prose into an indented code block.
    // A line consisting only of spaces is also deliberately unproven.
    if (leadingSpaces >= 4 || (lineEnd > lineStart && leadingSpaces == lineEnd - lineStart)) {
      return lastBoundary;
    }
    if (lineEnd == lineStart) {
      lastBoundary = newline + 1;
    }
    lineStart = newline + 1;
  }
  return lastBoundary;
}

bool IncrementalParser::appendCommitted(std::string_view source, size_t newBoundary) {
  const std::string_view region = source.substr(committedBytes_, newBoundary - committedBytes_);
  const ParseResult parsed = parseMarkdown(region);
  counters_.bytesParsed += region.size();
  if (parsed.status != ParseStatus::Ok) {
    return false;
  }
  const auto ranges = paragraphRanges(region);
  if (ranges.size() != parsed.document->blocks.size() ||
      committed_.size() + ranges.size() > kMaxCommittedBlocks || !parsed.document->links.empty()) {
    return false;
  }
  Utf16OffsetMap offsets(region);
  std::vector<Block> additions;
  additions.reserve(ranges.size());
  for (size_t i = 0; i < ranges.size(); i++) {
    Block block = parsed.document->blocks[i];
    if (block.kind != BlockKind::Paragraph || !block.containers.empty() || !block.runs.empty()) {
      return false;
    }
    block.identity = nextBlockId_++;
    block.sourceRange = {committedUtf16_ + offsets.toUtf16(ranges[i].start),
                         committedUtf16_ + offsets.toUtf16(ranges[i].end)};
    if (!committed_.empty() || !additions.empty()) {
      block.spacingBefore = BlockSpacing::Normal;
    }
    additions.push_back(std::move(block));
  }
  committed_.insert(committed_.end(), std::make_move_iterator(additions.begin()),
                    std::make_move_iterator(additions.end()));
  committedBytes_ = newBoundary;
  committedUtf16_ += offsets.toUtf16(region.size());
  counters_.committedBlocks += ranges.size();
  return true;
}

ParseResult IncrementalParser::update(std::string_view source, bool appendOnly, bool finished) {
  counters_.updates++;
  if (source.size() > kMaxSourceBytes) {
    return fullParse(source, ParseFallback::InvalidSource);
  }
  if (mode_ == ParseMode::FullParse) {
    return fullParse(source, ParseFallback::Count);
  }
  if (!appendOnly || source.size() < source_.size() || source.substr(0, source_.size()) != source_) {
    return fullParse(source, ParseFallback::NonAppend);
  }
  if (finished) {
    return fullParse(source, ParseFallback::Finished);
  }
  if (document_ && source == source_) {
    return {ParseStatus::Ok, document_, {}};
  }

  if (openFenceFast_ && source.size() > source_.size() &&
      source.size() - 4 <= kMaxRetainedTailBytes &&
      safeFenceContent(source.substr(source_.size()))) {
    appendUtf8(rawCodeText_, source.substr(source_.size()));
    auto next = std::make_shared<Document>(*document_);
    next->id = allocateDocumentId();
    next->blocks[0].text = rawCodeText_;
    if (!next->blocks[0].text.empty() && next->blocks[0].text.back() == u'\n') {
      next->blocks[0].text.pop_back();
    }
    source_.assign(source);
    document_ = next;
    counters_.openFenceFastUpdates++;
    return {ParseStatus::Ok, next, {}};
  }

  const size_t boundary = findSafeBoundary(source);
  if (boundary > committedBytes_ && !appendCommitted(source, boundary)) {
    return fullParse(source, ParseFallback::InvariantFailure);
  }
  if (source.size() - committedBytes_ > kMaxRetainedTailBytes) {
    return fullParse(source, ParseFallback::TailLimit);
  }
  if (committedBytes_ == 0) {
    return fullParse(source, ParseFallback::InitialOrUnprovenBoundary);
  }

  const std::string_view tail = source.substr(committedBytes_);
  ParseResult result = parseMarkdown(tail);
  counters_.tailParses++;
  counters_.bytesParsed += tail.size();
  if (result.status != ParseStatus::Ok) {
    return result;
  }

  auto combined = std::make_shared<Document>(*result.document);
  const size_t tailStart = committed_.size();
  combined->blocks.insert(combined->blocks.begin(), committed_.begin(), committed_.end());
  if (tailStart > 0 && combined->blocks.size() > tailStart) {
    combined->blocks[tailStart].spacingBefore = BlockSpacing::Normal;
  }
  for (auto &link : combined->links) {
    if (link.range.end > link.range.start) {
      link.range.start += committedUtf16_;
      link.range.end += committedUtf16_;
    }
  }
  counters_.cacheHits += committed_.size();
  source_.assign(source);
  document_ = combined;
  result.document = combined;
  return result;
}

} // namespace downstream
