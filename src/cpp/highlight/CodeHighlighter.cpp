#include "CodeHighlighter.h"

#include "StreamLexer.h"
#include "Tokens.h"
#include "Unicode.h"

#include <algorithm>
#include <chrono>

namespace downstream {
namespace {

/// Lines lexed between budget checks. Small enough that a stop lands within a
/// few lines, large enough that the lines cost more than a clock read.
constexpr size_t kWindowLines = 32;

// Relexing an unfinished 60 KiB line for every 16-byte append is quadratic.
// Beyond this length, live parser snapshots refresh at geometric growth
// points. A single lexer call can still exceed its time budget.
constexpr size_t kLiveLineRefreshThreshold = 4096;

bool isHighSurrogate(char16_t unit) { return unit >= 0xd800 && unit <= 0xdbff; }
bool isLowSurrogate(char16_t unit) { return unit >= 0xdc00 && unit <= 0xdfff; }

/// The length utf16ToUtf8 gives `text`, without converting it. A lone
/// surrogate becomes U+FFFD, three bytes.
size_t utf8Length(std::u16string_view text) {
  size_t length = 0;
  for (size_t index = 0; index < text.size(); index++) {
    const char16_t unit = text[index];
    if (unit < 0x80) {
      length += 1;
    } else if (unit < 0x800) {
      length += 2;
    } else if (isHighSurrogate(unit) && index + 1 < text.size() && isLowSurrogate(text[index + 1])) {
      length += 4;
      index++;
    } else {
      length += 3;
    }
  }
  return length;
}

bool isBlank(char16_t unit) { return unit == u' ' || unit == u'\t' || unit == u'\r' || unit == u'\n' || unit == u'\f'; }

/// Drops the runs that reach into `[utf16, ...)`, so the region a pass is about
/// to rebuild holds nothing from the last pass. A run that straddles the offset
/// is cut there, which never splits a token: the offset is a line start.
void dropRunsFrom(std::vector<TokenRun> *runs, uint32_t utf16) {
  while (!runs->empty() && runs->back().end > utf16) {
    runs->back().end = std::min(runs->back().end, utf16);
    if (runs->back().end <= runs->back().start) {
      runs->pop_back();
    } else {
      break;
    }
  }
}

/// Appends the runs for one lexed chunk. `records` are the module's
/// (endUtf16, tokenId) pairs counted from `base`, the chunk's offset in `text`.
///
/// The module extends a token over the whitespace that follows it, and a
/// line's indentation can lead its first token, so a run covers only the
/// token's non-blank units and the gaps stay plain. Whitespace inside a token,
/// as in a comment or a string, is the token's own.
void appendRecords(const std::vector<uint32_t> &records, std::u16string_view text, uint32_t base,
                   std::vector<TokenRun> *runs) {
  const uint32_t limit = static_cast<uint32_t>(text.size());
  uint32_t start = base;
  for (size_t index = 0; index + 1 < records.size(); index += 2) {
    const uint32_t end = std::min(limit, base + records[index]);
    if (end <= start) {
      continue;
    }
    const uint32_t id = records[index + 1];
    const TokenClass token = id == StreamLexer::kLineEnd ? TokenClass::Plain : tokenClassFor(id);
    if (token != TokenClass::Plain) {
      uint32_t first = start;
      uint32_t last = end;
      while (first < last && isBlank(text[first])) {
        first++;
      }
      while (last > first && isBlank(text[last - 1])) {
        last--;
      }
      if (last > first) {
        if (!runs->empty() && runs->back().end == first && runs->back().token == token) {
          runs->back().end = last;
        } else {
          runs->push_back({first, last, token});
        }
      }
    }
    start = end;
  }
}

class Highlighter final : public CodeHighlighter {
public:
  void tokenize(std::string_view info, std::u16string_view text, HighlightState *state, std::vector<TokenRun> *runs,
                double budgetMs, bool settled) override {
    invalidateLineage(state);
    if (settled && budgetMs > 0) {
      tokenizeWhole(info, text, state, runs);
      return;
    }
    tokenizeImpl(info, text, state, runs, budgetMs, false, false, false);
    if (settled && state->lastStop == HighlightStop::Complete) {
      const uint64_t bytes = state->bytesLexed;
      const uint64_t built = state->runsBuilt;
      const uint64_t copied = state->textBytesCopied;
      tokenizeWhole(info, text, state, runs);
      state->bytesLexed += bytes;
      state->runsBuilt += built;
      state->textBytesCopied += copied;
    }
  }

  uint64_t lexersCreated() const override { return StreamLexer::created(); }

private:
  void tokenizeWhole(std::string_view info, std::u16string_view text, HighlightState *state,
                     std::vector<TokenRun> *runs) {
    const Language *language = findLanguage(info);
    if (language == nullptr || text.empty() || text.size() > kMaxHighlightBytes) {
      skip(state, runs, nullptr);
      return;
    }
    const bool applies = state->applies(language->alias, text);
    if (applies && state->canonical && state->text.size() == text.size()) {
      state->resumed = true;
      state->bytesLexed = state->runsBuilt = state->textBytesCopied = 0;
      *runs = state->runs;
      state->tokenBytesCopied = runs->size() * sizeof(TokenRun);
      return;
    }
    const std::string bytes = utf16ToUtf8(text);
    if (bytes.size() > kMaxHighlightBytes) {
      skip(state, runs, nullptr);
      state->overByteLimit = true;
      return;
    }
    // Whole output replaces every run and starts from an empty lexer. A saved
    // line-stream checkpoint cannot reproduce lookahead across its boundary.
    StreamLexer *lexer = StreamLexer::current();
    auto &records = scratch();
    if (lexer == nullptr || !lexer->start(language->id, nullptr) || !lexer->lex(bytes, &records)) {
      fail(state, runs, nullptr);
      return;
    }
    // text can view state->text during a budgeted completion. Copy it before
    // clear invalidates that view.
    std::u16string owned(text);
    state->clear();
    state->language = language->alias;
    state->languageId = language->id;
    state->text = std::move(owned);
    state->utf8Length = bytes.size();
    const size_t newline = state->text.rfind(u'\n');
    state->lineStartUtf16 = newline == std::u16string::npos ? 0 : newline + 1;
    appendRecords(records, state->text, 0, &state->runs);
    state->resumed = applies;
    state->canonical = true;
    state->lastStop = HighlightStop::Complete;
    state->bytesLexed = bytes.size();
    state->runsBuilt = state->runs.size();
    state->textBytesCopied = bytes.size() + state->text.size() * sizeof(char16_t);
    *runs = state->runs;
    state->tokenBytesCopied = runs->size() * sizeof(TokenRun);
  }

  void tokenizeTrustedAppend(std::string_view info, std::u16string_view text, HighlightState *state,
                             std::vector<TokenRun> *runs, double budgetMs, bool live,
                             bool terminalLineComplete) override {
    tokenizeImpl(info, text, state, runs, budgetMs, true, live, terminalLineComplete);
  }
  void tokenizeTrustedDelta(std::string_view info, const SnapshotText &, std::u16string_view delta,
                            HighlightState *state, SnapshotVector<TokenRun> *runs, double budgetMs, bool live,
                            bool terminalLineComplete) override {
    tokenizeImpl(info, state->text, state, nullptr, budgetMs, true, live, terminalLineComplete, &delta, runs);
  }

  static void skip(HighlightState *state, std::vector<TokenRun> *runs, SnapshotVector<TokenRun> *publication) {
    state->clear();
    if (runs) runs->clear();
    if (publication) publication->clear();
    state->lastStop = HighlightStop::Skipped;
  }

  void tokenizeImpl(std::string_view info, std::u16string_view text, HighlightState *state,
                    std::vector<TokenRun> *runs, double budgetMs, bool trustedAppend, bool live,
                    bool terminalLineComplete, const std::u16string_view *delta = nullptr,
                    SnapshotVector<TokenRun> *publication = nullptr) {
    state->resumed = false;
    state->canonical = false;
    state->bytesLexed = state->runsBuilt = state->tokenBytesCopied = state->textBytesCopied = 0;
    const HighlightStop previousStop = state->lastStop;
    const Language *language = findLanguage(info);
    // UTF-8 is never shorter than UTF-16, so an over-long text is over the
    // limit before any of it is converted.
    const size_t textLength = text.size() + (delta ? delta->size() : 0);
    if (language == nullptr || textLength == 0 || textLength > kMaxHighlightBytes) {
      skip(state, runs, publication);
      return;
    }

    // The state is reused only when the block grew, so a stream pays for its
    // last line and nothing else.
    const bool appendApplies = trustedAppend && !state->language.empty() && state->language == language->alias &&
        state->text.size() <= textLength;
    if (appendApplies || (!delta && state->applies(language->alias, text))) {
      state->resumed = true;
      const std::u16string_view added = delta ? *delta : text.substr(state->text.size());
      // A pair split across appends was counted as a replacement for its high
      // half; whole, it is four bytes rather than three and three.
      const bool joinsSurrogate = !state->text.empty() && !added.empty() && isHighSurrogate(state->text.back()) &&
          isLowSurrogate(added.front());
      state->utf8Length += utf8Length(added) - (joinsSurrogate ? 2 : 0);
      const size_t newline = added.rfind(u'\n');
      if (newline != std::u16string_view::npos) {
        state->lineStartUtf16 = state->text.size() + newline + 1;
      }
      state->text.append(added);
      state->textBytesCopied = added.size() * sizeof(char16_t);
    } else {
      // A language change or a rewrite starts the block over. Copy the old
      // state before clear(), since delta calls view that very state.
      std::u16string rebuilt;
      if (delta) {
        rebuilt.assign(text);
        rebuilt.append(*delta);
        text = rebuilt;
      }
      state->clear();
      state->language = language->alias;
      state->languageId = language->id;
      state->text.assign(text);
      state->textBytesCopied = text.size() * sizeof(char16_t);
      state->utf8Length = utf8Length(text);
      const size_t newline = text.rfind(u'\n');
      state->lineStartUtf16 = newline == std::u16string_view::npos ? 0 : newline + 1;
    }
    if (state->utf8Length > kMaxHighlightBytes) {
      skip(state, runs, publication);
      state->overByteLimit = true;
      return;
    }

    const size_t lineLength = state->text.size() - state->lineStartUtf16;
    const bool sameUnfinishedLine = state->resumed && state->lineStartUtf16 == state->liveLineStart;
    if (trustedAppend && live && !terminalLineComplete && sameUnfinishedLine &&
        previousStop != HighlightStop::Budget && lineLength > kLiveLineRefreshThreshold &&
        lineLength < state->nextLiveLineRefresh) {
      // tokenizeAppend's lineage guarantees that runs already contains the
      // previous snapshot's tokens. Keep those provisional tokens without
      // copying them again. The newly appended text remains plain; publication
      // keeps the preceding immutable vector when this pass is deferred.
      state->lastStop = HighlightStop::Deferred;
      return;
    }

    // Only the unsettled stretch is converted: whatever the budget left of the
    // complete lines, then the unfinished last line. A surrogate pair split
    // across appends is whole again here, because the stretch is converted
    // from the text rather than kept as bytes.
    const std::string utf8 = utf16ToUtf8(std::u16string_view(state->text).substr(state->finalUtf16));
    state->textBytesCopied += utf8.size();
    const size_t newline = utf8.rfind('\n');
    const size_t lineStart = newline == std::string::npos ? 0 : newline + 1;

    // The last line is rebuilt every pass, so what the previous pass made of it
    // goes before the new runs are added. The last retained run can merge with
    // the rebuilt suffix, even when it ends exactly at the settled boundary, so
    // publish that run again as well.
    const size_t retainedRuns = state->resumed ? static_cast<size_t>(std::lower_bound(
        state->runs.begin(), state->runs.end(), state->finalUtf16,
        [](const TokenRun &run, uint32_t boundary) { return run.end < boundary; }) - state->runs.begin()) : 0;
    dropRunsFrom(&state->runs, state->finalUtf16);

    StreamLexer *lexer = StreamLexer::current();
    if (lexer == nullptr || !lexer->start(state->languageId, state->finalEnd > 0 ? &state->resume : nullptr)) {
      fail(state, runs, publication);
      return;
    }
    std::vector<uint32_t> &records = scratch();
    const auto started = std::chrono::steady_clock::now();
    size_t at = 0;
    uint32_t atUtf16 = state->finalUtf16;
    size_t lines = 0;
    bool stopped = false;
    while (at < utf8.size()) {
      // One line per call. The lexer looks ahead to the end of its input, so a
      // chunk of several lines could color one line by the next, and a block's
      // tokens would depend on where its passes happened to stop.
      const size_t newlineAt = utf8.find('\n', at);
      const size_t end = newlineAt == std::string::npos ? utf8.size() : newlineAt + 1;
      const std::string_view chunk = std::string_view(utf8).substr(at, end - at);
      if (!lexer->lex(chunk, &records)) {
        fail(state, runs, publication);
        return;
      }
      appendRecords(records, state->text, atUtf16, &state->runs);
      state->bytesLexed += chunk.size();
      atUtf16 += utf16Length(chunk);
      at = end;
      if (at == lineStart && at > 0) {
        // Everything before the unfinished line is final: no later chunk can
        // change a token there, so the stream is saved here and the pass after
        // this one resumes from it.
        if (!lexer->capture(&state->resume)) {
          fail(state, runs, publication);
          return;
        }
        state->finalEnd += at;
        state->finalUtf16 = atUtf16;
      }
      // The budget is checked every window of lines and before the unfinished
      // line, which is lexed whole.
      if (++lines % kWindowLines != 0 && at != lineStart) {
        continue;
      }
      const double spent =
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
      if (at < utf8.size() && spent >= budgetMs) {
        stopped = true;
        break;
      }
    }
    if (stopped && at < lineStart) {
      // The budget stopped inside the complete lines: settle what was lexed.
      if (!lexer->capture(&state->resume)) {
        fail(state, runs, publication);
        return;
      }
      state->finalEnd += at;
      state->finalUtf16 = atUtf16;
    }

    state->runsBuilt = state->runs.size() - retainedRuns;
    // A block whose last line has no newline yet is fully colored but not yet
    // final; only bytes the pass never reached mean it ran out of budget.
    state->lastStop = stopped ? HighlightStop::Budget : HighlightStop::Complete;
    if (publication) {
      const TokenRun *suffix = state->runs.empty() ? nullptr : state->runs.data() + retainedRuns;
      state->tokenBytesCopied = publication->replaceSuffix(
          retainedRuns, suffix, state->runs.size() - retainedRuns).payloadBytes;
    } else {
      *runs = state->runs;
      state->tokenBytesCopied = state->runs.size() * sizeof(TokenRun);
    }
    state->liveLineStart = state->lineStartUtf16;
    state->nextLiveLineRefresh = std::max(kLiveLineRefreshThreshold, lineLength * 2);
  }

  /// A trapped lexer cannot be trusted with another block, so the thread drops
  /// it, and this block stays plain rather than half-colored.
  static void fail(HighlightState *state, std::vector<TokenRun> *runs, SnapshotVector<TokenRun> *publication) {
    StreamLexer::discard();
    skip(state, runs, publication);
  }

  /// Per thread, because one highlighter serves the whole worker pool and every
  /// chunk's records land here rather than in a fresh allocation.
  static std::vector<uint32_t> &scratch() {
    static thread_local std::vector<uint32_t> records;
    return records;
  }
};

} // namespace

HighlightState::HighlightState() = default;
HighlightState::HighlightState(HighlightState &&other) noexcept = default;
HighlightState &HighlightState::operator=(HighlightState &&other) noexcept = default;
HighlightState::~HighlightState() = default;

/// True when this state describes a prefix of `next` for the same language, so
/// the block only grew and a resume is valid.
///
/// The comparison is over the whole block, so a pass costs O(block) even when it
/// only lexes the new tail. That is cheap next to lexing the block again, and it
/// is what makes the check sound: a same-length rewrite and a shorter text have
/// to be told apart from a genuine append, and only reading every unit can do
/// that. Streaming n units in small chunks is therefore O(n^2) in comparison
/// alone for general callers. The parser's verified open-fence path avoids the
/// comparison using exact snapshot lineage; arbitrary text still comes here.
bool HighlightState::applies(std::string_view alias, std::u16string_view next) const {
  return !language.empty() && language == alias && text.size() <= next.size() &&
      std::equal(text.begin(), text.end(), next.begin());
}

void HighlightState::clear() {
  sourceDocumentId_ = 0;
  resumed = false;
  lastStop = HighlightStop::Skipped;
  language.clear();
  languageId = 0;
  text.clear();
  utf8Length = lineStartUtf16 = 0;
  finalEnd = 0;
  finalUtf16 = 0;
  bytesLexed = runsBuilt = tokenBytesCopied = textBytesCopied = 0;
  nextLiveLineRefresh = liveLineStart = 0;
  overByteLimit = false;
  canonical = false;
  runs.clear();
  resume.clear();
}

void CodeHighlighter::tokenizeAppend(std::string_view info, const SnapshotText &text, std::u16string_view delta,
                                     HighlightState *state, SnapshotVector<TokenRun> *runs, double budgetMs,
                                     uint64_t previousDocumentId, size_t previousLength, uint64_t nextDocumentId,
                                     bool live, bool terminalLineComplete) {
  // Skipped blocks do not retain a lexer/text state. Recognize them before a
  // checked string-view conversion would flatten every growing snapshot.
  const bool retainedOversize = state->overByteLimit && previousDocumentId != 0 &&
      state->sourceDocumentId_ == previousDocumentId && previousLength + delta.size() == text.size();
  if (retainedOversize || findLanguage(info) == nullptr || text.empty() || text.size() > kMaxHighlightBytes) {
    state->clear();
    state->overByteLimit = retainedOversize;
    runs->clear();
    state->sourceDocumentId_ = nextDocumentId;
    return;
  }
  const bool lineage = previousDocumentId != 0 && state->sourceDocumentId_ == previousDocumentId &&
      state->text.size() == previousLength && previousLength + delta.size() == text.size();
  invalidateLineage(state);
  if (lineage && live) {
    tokenizeTrustedDelta(info, text, delta, state, runs, budgetMs, live, terminalLineComplete);
  } else {
    std::vector<TokenRun> next;
    tokenize(info, text, state, &next, budgetMs, !live);
    *runs = std::move(next);
  }
  state->sourceDocumentId_ = nextDocumentId;
}

CodeHighlighter &downstreamHighlighter() {
  static Highlighter highlighter;
  return highlighter;
}

} // namespace downstream
