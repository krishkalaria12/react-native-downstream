#pragma once

#include "CodeBlocks.h"
#include "MarkdownParser.h"
#include "Preview.h"

#include <cstddef>
#include <cstdint>
#include <memory>
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
  Finished,
  Count,
};

struct IncrementalCounters {
  uint64_t updates{0};
  uint64_t bytesParsed{0};
  uint64_t fullParses{0};
  uint64_t tailParses{0};
  uint64_t openFenceFastUpdates{0};
  uint64_t commitAttempts{0};
  uint64_t rejectedCommits{0};
  uint64_t committedBlocks{0};
  uint64_t cacheHits{0};
  /// Code blocks a highlighting pass colored, and how many of those passes
  /// continued the previous one instead of starting the block over.
  uint64_t blocksHighlighted{0};
  uint64_t highlightResumes{0};
  uint64_t highlight[kHighlightStopCount]{};

  uint64_t highlightCount(HighlightStop reason) const {
    return highlight[static_cast<size_t>(reason)];
  }
  uint64_t fallback[static_cast<size_t>(ParseFallback::Count)]{};

  uint64_t fallbackCount(ParseFallback reason) const {
    return fallback[static_cast<size_t>(reason)];
  }
};

/// One session's parser state and canonical source. Calls are serialized by
/// the owning session. Returned documents are immutable.
///
/// Full mode parses the whole source every time. Leading blocks that equal
/// the previous snapshot's keep a stable identity, so platforms reuse their
/// rendering and lay out only the changed tail.
///
/// Incremental mode commits a source prefix once md4c shows that no later
/// input can change it, and then parses only the remaining tail. Committed
/// blocks live in storage shared by every later snapshot, so an update copies
/// only its tail. Their identities are unique across all parsers and never
/// reused, so an identity always names the same block content.
class IncrementalParser {
public:
  explicit IncrementalParser(ParseMode mode = ParseMode::FullParse) : mode_(mode) {}

  /// Appends accepted bytes to the canonical source.
  void append(std::string_view chunk);
  /// Replaces the canonical source, as after a stream reset.
  void replace(std::string_view source);
  /// Parses the source as it stands. A finished parse always renders the
  /// complete document; otherwise only the active tail may be parsed.
  ParseResult parse(bool finished);
  /// Clears the source, parser state, and counters.
  void reset();

  ParseMode mode() const { return mode_; }
  std::string_view source() const { return source_; }
  DocumentHandle document() const { return document_; }
  IncrementalCounters counters() const { return counters_; }
  size_t committedBytes() const { return committedBytes_; }

  /// Opts the unstable tail into provisional styling. Off by default, which is
  /// why every open span renders literally unless a caller asks for this.
  ///
  /// Only the tail is ever repaired, and a repair is applied only when parsing
  /// it proves the repair changed nothing but the block it aimed at. Committed
  /// blocks and the canonical source are never touched, and `finished` ignores
  /// the policy entirely, so the settled document is always a parse of the
  /// bytes the writer actually sent.
  ///
  /// Safe to call at any time, including while a stream is open. It forces one
  /// more parse, because otherwise a session with no pending input would
  /// publish the snapshot it already has and the new policy would not appear.
  void setPreview(remend::PreviewOptions options) {
    preview_ = remend::Preview(options);
    previewDirty_ = true;
  }
  bool previewEnabled() const { return preview_.enabled(); }
  /// Cumulative across every parse since the last reset, so a caller can see
  /// how often the pass fired without instrumenting each snapshot.
  remend::PreviewStats previewStats() const { return previewStats_; }

private:
  /// `previewable` is false for a finished stream and a rejected source, which
  /// are the documents the provisional-styling pass must never touch.
  ParseResult fullParse(ParseFallback reason, bool previewable);
  /// Full mode: gives leading blocks that are unchanged since the previous
  /// snapshot a stable identity, so platforms reuse their rendering.
  void carryIdentities(ParseResult &result);
  /// Adds one preview pass to the stats and counters, probes included.
  void notePreview(const remend::PreviewResult &preview);
  /// Adds one set of highlighting passes to the counters.
  void noteHighlight(const HighlightStats &stats);
  ParseResult parseTail();
  /// Colors the tail code block, resuming from the last pass. A tail that is
  /// not code clears the state, which is what releases its lexer.
  void highlightTail(Document *document);
  void publish(DocumentHandle document);
  void tryCommit();
  bool commitRegion(size_t boundary);
  bool appendOpenFence();
  void watchOpenFence(std::string_view source, size_t from, size_t blocksBefore);
  size_t committedCount() const;
  void clearCommitted();
  void resetScan();

  ParseMode mode_;
  std::string source_;
  DocumentHandle document_;
  // Source length that document_ reflects, and whether the source was
  // replaced since then.
  size_t parsedBytes_{0};
  bool replaced_{false};

  std::shared_ptr<const SharedList<Block>::Prefix> committedBlocks_;
  std::shared_ptr<const SharedList<Link>::Prefix> committedLinks_;
  size_t committedBytes_{0};
  uint32_t committedUtf16_{0};
  uint32_t committedContainers_{0};
  uint32_t committedTables_{0};

  // Complete lines are scanned once for commit candidates. A rejected
  // candidate backs off in proportion to the uncommitted region.
  size_t scannedBytes_{0};
  size_t lastBlankEnd_{0};
  size_t nextBlankAttempt_{0};
  size_t nextLineAttempt_{0};
  size_t lineAttempted_{0};

  // A trailing open fence accepts plain content without reparsing.
  bool openFence_{false};
  bool fencePendingNewline_{false};

  // The tail code block's highlighting resume state, released on commit, on a
  // full parse, and whenever the tail is no longer code.
  HighlightState highlight_;

  // Provisional styling for the tail. Inert until a caller sets it.
  remend::Preview preview_;
  remend::PreviewStats previewStats_{};
  // A new policy has to reach the reader even if no new input arrived.
  bool previewDirty_{false};

  IncrementalCounters counters_;
};

} // namespace downstream
