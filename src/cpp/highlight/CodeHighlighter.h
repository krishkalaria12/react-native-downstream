#pragma once

#include "CodeDocument.h"
#include "Document.h"
#include "LanguageRegistry.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace downstream {

/// Per-block engine state. Defined in CodeHighlighter.cpp because it owns a
/// Lexilla lexer, whose namespace is renamed at compile time.
struct HighlightEngine;

/// Why the last highlighting pass ended, for the counters the Bench screen
/// shows and for the tests that drive a budget to zero.
enum class HighlightStop : uint8_t {
  /// Every byte of the block was colored.
  Complete = 0,
  /// The block has no known language, or is over the highlight limit.
  Skipped,
  /// The pass spent its budget, so the rest of the block stays plain until the
  /// next one resumes it.
  Budget,
  /// A long unfinished line kept its provisional tokens until the next live
  /// refresh. Newlines and settled/checked calls always refresh it.
  Deferred,
  Count,
};

inline constexpr size_t kHighlightStopCount = static_cast<size_t>(HighlightStop::Count);

/// Resume state for one code block whose text is still growing.
///
/// Lexilla lexers carry per-line state, so a block that only gained bytes at
/// its end needs only its last line lexed again. The state keeps the document,
/// its styles and line states, and the runs for every line before that one,
/// which is why a long stream costs one line per snapshot rather than the whole
/// block.
///
/// A state only applies to text that extends what it was built from, and
/// `CodeHighlighter::tokenize` checks that, so a caller may hand it any text in
/// any order: a full reparse and a stream reset both start the block over. A
/// state is not thread-safe; one belongs to one code block, and a block belongs
/// to one document.
struct HighlightState {
  HighlightState();
  HighlightState(const HighlightState &) = delete;
  HighlightState &operator=(const HighlightState &) = delete;
  HighlightState(HighlightState &&other) noexcept;
  HighlightState &operator=(HighlightState &&other) noexcept;
  ~HighlightState();

  /// The block's text as UTF-16, exactly as the parser produced it. General
  /// callers compare it against the new text; the parser's verified append
  /// path may instead establish the same relationship by snapshot lineage.
  bool applies(std::string_view alias, std::u16string_view text) const;
  void clear();

  /// Info-string alias the state was built for, empty when it holds nothing.
  std::string language;
  /// The block text the state was built from, as UTF-16.
  std::u16string text;
  /// Byte offset into the lexer's UTF-8 text where the runs stop, which is the
  /// start of the last line when the text has no final newline, so a token the
  /// next chunk changes corrects itself.
  size_t finalEnd{0};
  /// UTF-16 length of the lexed text up to finalEnd, so appending need not
  /// rescan what is already settled.
  uint32_t finalUtf16{0};
  /// Runs covering the lexed text. Sorted, non-overlapping, and never covering
  /// Plain text.
  std::vector<TokenRun> runs;
  /// The lexer's view of the whole text, in UTF-8, kept so a lexer that looks
  /// back a line reads the styles and line states it would have found in an
  /// editor.
  CodeDocument document;
  /// The lexer configured for this block, or null when none could be made.
  std::unique_ptr<HighlightEngine> engine;
  /// Why the last pass stopped, and whether it continued the previous one
  /// rather than starting the block over.
  HighlightStop lastStop{HighlightStop::Skipped};
  bool resumed{false};
  /// Work requested by the last pass. bytesLexed counts Lex() input windows,
  /// not any additional backward reads performed inside a lexer.
  uint64_t bytesLexed{0};
  uint64_t runsBuilt{0};
  /// Token payload copied into immutable publications, including the retained
  /// fragment of a partially rewritten page. Shared page-index nodes are not
  /// token payload; checked vector callers still copy their whole result.
  uint64_t tokenBytesCopied{0};
  uint64_t textBytesCopied{0};

  /// Only used by the parser's live append policy. This does not make any
  /// tokens on the unfinished line immutable.
  size_t nextLiveLineRefresh{0};
  size_t liveLineStart{0};
  /// A verified append cannot bring an oversized UTF-8 block back below the
  /// byte limit, even when its UTF-16 length is still below that limit.
  bool overByteLimit{false};

private:
  friend class CodeHighlighter;
  // Only the parser's verified open-fence path establishes this association.
  // A general tokenize call invalidates it before inspecting arbitrary text.
  uint64_t sourceDocumentId_{0};
};

/// Tokenizes fenced code blocks.
///
/// The interface is engine-agnostic on purpose: a tree-sitter or TextMate
/// engine can replace the Lexilla one for the languages where fidelity matters
/// more than binary size, without the parser or the platforms changing.
class CodeHighlighter {
public:
  virtual ~CodeHighlighter() = default;

  /// Fills `runs` with the tokens of one code block, resuming from `state`
  /// when it still applies. `info` is the fence's info string and `text` the
  /// block's UTF-16 text. Leaves `runs` empty and clears `state` when the
  /// language is unknown or the block is over the highlight limit.
  ///
  /// Stops early when `budgetMs` is spent, which leaves the rest of the block
  /// plain and resumes on the next call. A block whose text has ended in a
  /// newline is lexed whole, so a settled document's tokens are the same
  /// whether it arrived in one piece or a thousand.
  virtual void tokenize(std::string_view info, std::u16string_view text, HighlightState *state,
                        std::vector<TokenRun> *runs, double budgetMs) = 0;

  /// Lexers created since the process started. A document with no code block
  /// never creates one, which the isolation test checks.
  virtual uint64_t lexersCreated() const = 0;

private:
  friend class IncrementalParser;
  /// The parser retained the previous snapshot's immutable tokens and appended
  /// its text pages. delta contains only the appended UTF-16 code units.
  /// Exact snapshot lineage permits skipping the whole-prefix comparison.
  /// Live calls can defer a long unfinished line's provisional styling until
  /// its length doubles. terminalLineComplete accounts for md4c stripping a
  /// source newline; it forces a refresh. Other callers use tokenize.
  void tokenizeAppend(std::string_view info, const SnapshotText &text, std::u16string_view delta, HighlightState *state,
                      SnapshotVector<TokenRun> *runs, double budgetMs, uint64_t previousDocumentId,
                      size_t previousLength, uint64_t nextDocumentId, bool live = true,
                      bool terminalLineComplete = false);
  virtual void tokenizeTrustedAppend(std::string_view info, std::u16string_view text, HighlightState *state,
                                     std::vector<TokenRun> *runs, double budgetMs, bool, bool) {
    tokenize(info, text, state, runs, budgetMs);
  }
  virtual void tokenizeTrustedDelta(std::string_view info, const SnapshotText &text,
                                    std::u16string_view, HighlightState *state,
                                    SnapshotVector<TokenRun> *runs, double budgetMs, bool live,
                                    bool terminalLineComplete) {
    std::vector<TokenRun> next;
    tokenizeTrustedAppend(info, text, state, &next, budgetMs, live, terminalLineComplete);
    *runs = std::move(next);
  }

protected:
  static void invalidateLineage(HighlightState *state) { state->sourceDocumentId_ = 0; }
};

/// The process-wide Lexilla highlighter. One per process is enough: a lexer
/// holds no document state, and a `HighlightState` owns the one lexer it needs.
CodeHighlighter &downstreamHighlighter();

} // namespace downstream
