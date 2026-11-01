#pragma once

#include "CodeBlocks.h"
#include "MarkdownParser.h"
#include "Preview.h"
#include "math/MathBlocks.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
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
  uint64_t plainParagraphFastUpdates{0};
  uint64_t commitAttempts{0};
  uint64_t rejectedCommits{0};
  uint64_t committedBlocks{0};
  uint64_t cacheHits{0};
  /// Code blocks a highlighting pass colored, and how many of those passes
  /// continued the previous one instead of starting the block over.
  uint64_t blocksHighlighted{0};
  uint64_t highlightResumes{0};
  uint64_t highlightBytesLexed{0};
  uint64_t highlightRunsBuilt{0};
  uint64_t highlightTokenBytesCopied{0};
  uint64_t highlightTextBytesCopied{0};
  /// Immutable active-fence pages and persistent index nodes copied by append.
  uint64_t fenceSnapshotStorageBytesCopied{0};
  uint64_t fenceAppendScanBytes{0};
  uint64_t highlight[kHighlightStopCount]{};

  uint64_t highlightCount(HighlightStop reason) const {
    return highlight[static_cast<size_t>(reason)];
  }

  /// What the math layout passes did, and the one number a budget test wants:
  /// how many formulas reached the engine rather than the cache.
  uint64_t formulasLaidOut{0};
  /// Formulas that kept a previous snapshot's layout because they have not
  /// finished arriving. A large value on a slow stream is a sign the cache is
  /// not being hit, which is the thing that would make the pass expensive.
  uint64_t mathLastGoodKept{0};
  /// Formulas refused by the size caps before the engine was asked.
  uint64_t mathSkippedTooLarge{0};
  /// Formulas the engine or the decoder rejected.
  uint64_t mathFailed{0};
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
///
/// Closed blocks with unresolved references may also be shared provisionally.
/// They have identities, so renderers reuse their layout, but are not settled.
/// New definition markers revoke that provisional prefix before the next parse,
/// leaving the strict prefix intact; reparsed blocks take fresh identities. Confirmed link definitions are replayed in source order for tails;
/// footnotes, bracket math and preview keep the canonical full-parse proof.
class IncrementalParser {
public:
  explicit IncrementalParser(ParseMode mode = ParseMode::FullParse,
                             ParseOptions parse = ParseOptions())
      : mode_(mode), parse_(parse) {}

  /// The parse options this session was created with.
  ///
  /// Read once, like the mode, because parse options are part of a source's
  /// identity: a session whose delimiters changed under it would parse the same
  /// bytes two ways, and the reader would see a formula change shape without the
  /// document changing. JavaScript binds the view's options to a native session,
  /// keeps them across reset(), and rebuilds the session when the policy changes.
  const ParseOptions &options() const { return parse_; }
  void setParse(ParseOptions parse) {
    if (parse_ == parse) return;
    parse_ = parse;
    clearCommitted();
    dependencyScannedBytes_ = 0;
    referenceDependentSource_ = false;
    openFence_ = false;
    highlight_.clear();
    replaced_ = true;
  }

  /// Appends accepted bytes to the canonical source.
  void append(std::string_view chunk);
  /// Replaces the canonical source, as after a stream reset.
  void replace(std::string_view source);
  /// Parses the source as it stands. Finish settles the complete canonical
  /// document, reusing a proven dependency-free prefix when possible.
  ParseResult parse(bool finished);
  /// Clears the source, parser state, and counters.
  void reset();

  ParseMode mode() const { return mode_; }
  std::string_view source() const { return source_; }
  DocumentHandle document() const { return document_; }
  IncrementalCounters counters() const { return counters_; }
  size_t committedBytes() const {
    size_t bytes = committedBytes_;
    if (referencePrefix_) bytes = std::min(bytes, referencePrefix_->bytes);
    if (listPrefix_) bytes = std::min(bytes, listPrefix_->bytes);
    return bytes;
  }
  /// Source prefix reused provisionally until a new reference dependency
  /// arrives or an open list it ends inside turns loose. Unlike
  /// committedBytes(), this prefix can be revoked.
  size_t reusedBytes() const { return committedBytes_; }

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
    // The preview parses the tail on its own, with no list lead to parse it
    // after, so items of an open list go back to the tail.
    if (preview_.enabled()) invalidateListPrefix();
  }
  bool previewEnabled() const { return preview_.enabled(); }
  /// Cumulative across every parse since the last reset, so a caller can see
  /// how often the pass fired without instrumenting each snapshot.
  remend::PreviewStats previewStats() const { return previewStats_; }

private:
  /// `previewable` is false for a finished stream and a rejected source, which
  /// are the documents the provisional-styling pass must never touch.
  /// `settled` is true only for a finished stream, whose blocks no later input
  /// can change.
  ParseResult fullParse(ParseFallback reason, bool previewable, bool settled);
  /// Full mode: gives leading blocks that are unchanged since the previous
  /// snapshot a stable identity, so platforms reuse their rendering.
  void carryIdentities(ParseResult &result);
  /// Adds one preview pass to the stats and counters, probes included.
  void notePreview(const remend::PreviewResult &preview);
  /// Adds one set of highlighting passes to the counters.
  void noteHighlight(const HighlightStats &stats);
  ParseResult parseTail();
  /// Strict dependency-free prefixes can finish from their canonical tail.
  bool canFinishTail() const;
  /// Colors the tail code block, resuming from the last pass. A tail that is
  /// not code clears the state, which is what releases its lexer.
  void highlightTail(Document *document);
/// Records the snapshot the view will show, with the two flags that are the
  /// document's rather than its blocks': how many leading blocks no later input
  /// can change, and whether a stream is still arriving.
  ///
  /// It answers the document it recorded, which is a copy when the flags had to
  /// change, so a caller returns that rather than the one it passed in.
  const DocumentHandle &publish(DocumentHandle document, uint32_t settled);
  /// Lays out the tail's formulas in place, keeping the last good layout for
  /// one that is still arriving.
  void layoutTail(Document *document);
  /// Adds one set of layout passes to the counters.
  void noteMath(const MathLayoutStats &stats);
  void tryCommit();
  bool commitRegion(size_t boundary);
  /// Of a region that ends inside a top-level list, how many leading blocks
  /// end before the list's last item and the byte where that item's line
  /// starts, or zero blocks when there is no such item to stop before.
  std::pair<size_t, size_t> finishedListItems(std::string_view region, const ParseResult &parsed) const;
  /// The synthetic item the tail is parsed after while `listContext_` is set,
  /// or empty.
  std::string listLead() const;
  /// Parses `region`, which starts at the committed boundary, with the
  /// definitions and list context the committed prefix leaves it.
  ParseResult parseAfterCommitted(std::string_view region);
  /// True when a parse made with the list lead left the list as tight as the
  /// committed items assumed. Otherwise those items have to be respaced.
  bool keepsListContext(const ParseResult &parsed) const;
  void invalidateListPrefix();
  bool appendOpenFence();
  bool appendPlainParagraph();
  void watchPlainParagraph();
  void watchOpenFence(std::string_view source, size_t from, size_t blocksBefore);
  size_t committedCount() const;
  size_t settledCount() const;
  void invalidateReferencePrefix();
  void scanReferenceDependencies();
  void clearCommitted();
  void resetScan();

  ParseMode mode_;
  ParseOptions parse_;
  std::string source_;
  DocumentHandle document_;
  // Source length that document_ reflects, and whether the source was
  // replaced since then.
  size_t parsedBytes_{0};
  bool replaced_{false};

  std::shared_ptr<const SharedList<Block>::Prefix> committedBlocks_;
  std::shared_ptr<const SharedList<Link>::Prefix> committedLinks_;
  // Images are committed with the blocks that name them, so a spliced parse
  // reads the same index either way.
  std::shared_ptr<const SharedList<Image>::Prefix> committedImages_;
  std::shared_ptr<const SharedList<Tag>::Prefix> committedTags_;
  size_t committedBytes_{0};
  uint32_t committedUtf16_{0};
  uint32_t committedContainers_{0};
  uint32_t committedTables_{0};

  // The top-level list the committed prefix ends inside, when it does. The
  // tail starts with that list's next item and is parsed after a one-item list
  // standing in for the committed ones: same marker, the number before the
  // tail's, and the same tightness. See `listLead`.
  struct ListContext {
    bool ordered;
    char mark;
    uint32_t number;
    bool tight;
  };
  std::optional<ListContext> listContext_;

  // The committed state at some earlier point, restored when a provisional
  // prefix after it is revoked.
  struct Checkpoint {
    std::shared_ptr<const SharedList<Block>::Prefix> blocks;
    std::shared_ptr<const SharedList<Link>::Prefix> links;
    std::shared_ptr<const SharedList<Image>::Prefix> images;
    std::shared_ptr<const SharedList<Tag>::Prefix> tags;
    size_t bytes;
    uint32_t utf16;
    uint32_t containers;
    uint32_t tables;
    std::string definitions;
    uint32_t referenceOutputUsed;
    std::optional<ListContext> list;
  };
  Checkpoint checkpoint() const;
  void restore(Checkpoint checkpoint);
  // A closed region with unresolved references can be reused provisionally.
  // Its blocks have identities but never count as settled. A new definition
  // marker or budget-limited expansion revokes this strict-prefix checkpoint.
  std::optional<Checkpoint> referencePrefix_;
  // Items of a list that is still open are reused provisionally too: a later
  // blank line can make the whole list loose and respace every item. Revoked
  // when that happens, and dropped once a commit ends after the list.
  std::optional<Checkpoint> listPrefix_;
  size_t dependencyScannedBytes_{0};
  bool referenceDependentSource_{false};
  // md4c-confirmed definition-only replay. Bounded and source-order preserving.
  std::string committedDefinitions_;
  uint32_t committedReferenceOutput_{0};
  bool previousReferenceLimited_{false};
  // A parser-confirmed unfinished top-level paragraph with restricted syntax.
  bool plainParagraph_{false};
  size_t plainParagraphStart_{0};
  size_t paragraphPendingSpaces_{0};

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
  size_t fenceLineStart_{0};

  // The tail code block's highlighting resume state, released on commit, on a
  // full parse, and whenever the tail is no longer code.
  HighlightState highlight_;

  // Provisional styling for the tail. Inert until a caller sets it.
  remend::Preview preview_;
  remend::PreviewStats previewStats_{};
  // A new policy has to reach the reader even if no new input arrived.
  bool previewDirty_{false};
  // Whether the pass now running is the stream's last. `publish` reads it, so a
  // snapshot's own `streaming` flag always answers for that pass rather than for
  // whichever one happened to set it.
  bool finished_{false};

  IncrementalCounters counters_;
};

} // namespace downstream
