#pragma once

#include "MarkdownParser.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace downstream {

enum class ParseMode : uint8_t { FullParse, IncrementalTail };

enum class ParseFallback : uint8_t {
  InitialOrUnprovenBoundary,
  NonAppend,
  TailLimit,
  InvalidSource,
  InvariantFailure,
  Finished,
  Count,
};

struct IncrementalCounters {
  uint64_t updates{0};
  uint64_t bytesParsed{0};
  uint64_t fullParses{0};
  uint64_t tailParses{0};
  uint64_t openFenceFastUpdates{0};
  uint64_t committedBlocks{0};
  uint64_t cacheHits{0};
  uint64_t fallback[static_cast<size_t>(ParseFallback::Count)]{};

  uint64_t fallbackCount(ParseFallback reason) const {
    return fallback[static_cast<size_t>(reason)];
  }
};

/// One session's parser state. Calls are serialized by the owning session.
/// The input is always the entire canonical source; appendOnly is a hint
/// verified against the previous input. Returned documents are immutable.
class IncrementalParser {
public:
  explicit IncrementalParser(ParseMode mode = ParseMode::FullParse) : mode_(mode) {}

  ParseResult update(std::string_view source, bool appendOnly, bool finished);
  void reset();

  ParseMode mode() const { return mode_; }
  DocumentHandle document() const { return document_; }
  IncrementalCounters counters() const { return counters_; }
  size_t committedBytes() const { return committedBytes_; }

private:
  ParseResult fullParse(std::string_view source, ParseFallback reason);
  bool appendCommitted(std::string_view source, size_t newBoundary);
  size_t findSafeBoundary(std::string_view source) const;

  ParseMode mode_;
  std::string source_;
  DocumentHandle document_;
  std::vector<Block> committed_;
  size_t committedBytes_{0};
  uint32_t committedUtf16_{0};
  uint64_t nextBlockId_{1};
  bool openFenceFast_{false};
  std::u16string rawCodeText_;
  IncrementalCounters counters_;
};

} // namespace downstream
