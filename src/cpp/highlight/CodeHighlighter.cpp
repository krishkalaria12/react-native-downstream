#include "CodeHighlighter.h"

#include "Unicode.h"

// CatalogueModules.h calls strcmp without including its declaration.
#include <cstring>

#include "ILexer.h"
#include "LexerModule.h"
// Lexilla's catalogue expects both of the above.
#include "CatalogueModules.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>

// The LexerModule objects the vendored lexers export. Only these are
// registered, so no other Lexilla lexer can be reached by name.
extern const Lexilla::LexerModule lmBash;
extern const Lexilla::LexerModule lmCPP;
extern const Lexilla::LexerModule lmCss;
extern const Lexilla::LexerModule lmDiff;
extern const Lexilla::LexerModule lmHTML;
extern const Lexilla::LexerModule lmJSON;
extern const Lexilla::LexerModule lmLua;
extern const Lexilla::LexerModule lmMarkdown;
extern const Lexilla::LexerModule lmPython;
extern const Lexilla::LexerModule lmRuby;
extern const Lexilla::LexerModule lmRust;
extern const Lexilla::LexerModule lmSQL;
extern const Lexilla::LexerModule lmTOML;
extern const Lexilla::LexerModule lmXML;
extern const Lexilla::LexerModule lmYAML;

namespace downstream {

/// One code block's lexer, configured from the language table. It is created
/// per block and lives as long as the block's resume state, which is what lets
/// a lexer keep the cross-line state it built while a stream grows.
struct HighlightEngine {
  Scintilla::ILexer5 *lexer{nullptr};
  std::array<TokenClass, 64> classes{};
  HighlightEngine() = default;
  HighlightEngine(const HighlightEngine &) = delete;
  HighlightEngine &operator=(const HighlightEngine &) = delete;
  ~HighlightEngine();
};

namespace {

using Scintilla::ILexer5;

/// Lexilla keeps a lexer's activity flag and its sub-styles in the high bits of
/// a style byte, and its own `MaskActive` masks them off the same way. We never
/// allocate sub-styles, so masking leaves exactly the SCE_ constant.
constexpr int kStyleMask = 0x3f;

/// Lines lexed between budget checks. Small enough that a stop lands within a
/// line or two, large enough that a window costs less than a clock read.
constexpr size_t kWindowLines = 32;

// Relexing an unfinished 60 KiB line for every 16-byte append is quadratic.
// Beyond this length, live parser snapshots refresh at geometric growth
// points. A single lexer call can still exceed its time budget.
constexpr size_t kLiveLineRefreshThreshold = 4096;

/// Lexers created since the process started. A document with no code block
/// leaves it at zero, which is what the isolation test checks.
std::atomic<uint64_t> gLexersCreated{0};

/// The vendored lexers, as Lexilla's own static catalogue.
const Lexilla::CatalogueModules &lexers() {
  static const Lexilla::CatalogueModules *catalogue = [] {
    auto *built = new Lexilla::CatalogueModules();
    built->AddLexerModules({&lmBash, &lmCPP, &lmCss, &lmDiff, &lmHTML, &lmJSON, &lmLua, &lmMarkdown,
                            &lmPython, &lmRuby, &lmRust, &lmSQL, &lmTOML, &lmXML, &lmYAML});
    return built;
  }();
  return *catalogue;
}

/// A lexer configured for one language, or null when the name is not vendored.
std::unique_ptr<HighlightEngine> createEngine(const LanguageTable &language) {
  for (size_t index = 0; index < lexers().Count(); index++) {
    if (language.lexer != lexers().Name(index)) {
      continue;
    }
    auto engine = std::make_unique<HighlightEngine>();
    engine->lexer = lexers().Create(index);
    if (engine->lexer == nullptr) {
      return nullptr;
    }
    for (const char *const *property = language.properties; *property != nullptr; property += 2) {
      engine->lexer->PropertySet(property[0], property[1]);
    }
    for (int set = 0; language.words[set] != nullptr && language.words[set][0] != '\0'; set++) {
      engine->lexer->WordListSet(set, language.words[set]);
    }
    for (const int32_t *entry = language.styles; *entry >= 0; entry += 2) {
      if (entry[0] <= kStyleMask) {
        engine->classes[entry[0]] = static_cast<TokenClass>(entry[1]);
      }
    }
    gLexersCreated.fetch_add(1, std::memory_order_relaxed);
    return engine;
  }
  return nullptr;
}

bool isHorizontalSpace(char value) {
  return value == ' ' || value == '\t';
}

bool isUpperAscii(char value) {
  return value >= 'A' && value <= 'Z';
}

/// The class a language's post-pass gives the identifier span `[start, end)`.
/// Lexilla's lexers do not tell a call, a property, and a type apart, which is
/// the largest visible gap against a TextMate theme; these three rules close
/// most of it for the price of two byte comparisons.
TokenClass promotedClass(const LanguageTable &language, std::string_view utf8, size_t lineStart, size_t start,
                         size_t end) {
  if ((language.marks & MarkCalls) != 0) {
    size_t next = end;
    while (next < utf8.size() && isHorizontalSpace(utf8[next])) {
      next++;
    }
    if (next < utf8.size() && utf8[next] == '(') {
      return TokenClass::Function;
    }
  }
  if ((language.marks & MarkProperties) != 0 && start > lineStart) {
    size_t previous = start;
    while (previous > lineStart && isHorizontalSpace(utf8[previous - 1])) {
      previous--;
    }
    if (previous > lineStart && utf8[previous - 1] == '.') {
      return TokenClass::Property;
    }
  }
  if ((language.marks & MarkTypes) != 0 && isUpperAscii(utf8[start])) {
    return TokenClass::Type;
  }
  return TokenClass::Plain;
}

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

/// Joins the cached runs and the ones a pass just built into one maximal list.
/// Only the join can repeat a class, so only the join is checked.
void appendRuns(std::vector<TokenRun> *runs, const std::vector<TokenRun> &fresh) {
  if (!fresh.empty() && !runs->empty()) {
    TokenRun &last = runs->back();
    if (last.end == fresh.front().start && last.token == fresh.front().token) {
      last.end = fresh.front().end;
      runs->insert(runs->end(), fresh.begin() + 1, fresh.end());
      return;
    }
  }
  runs->insert(runs->end(), fresh.begin(), fresh.end());
}

/// The runs for `[from, to)` in UTF-16 offsets counted from the block's start,
/// with the post-pass already applied.
///
/// Walks the region once. A style change closes the open run, and an
/// identifier span, which carries no run of its own, is classified as it closes
/// and appended then: every run already in `fresh` ends at or before the span,
/// and the next one starts at or after it, so the list stays sorted.
void buildRuns(std::string_view utf8, const CodeDocument &document, const LanguageTable &language,
               const std::array<TokenClass, 64> &classes, size_t from, size_t to, uint32_t baseUtf16,
               std::vector<TokenRun> *fresh) {
  const std::vector<char> &styles = document.styles();
  fresh->clear();
  const bool promote = language.identifier >= 0 && language.marks != MarkNone;
  const int identifier = language.identifier & kStyleMask;
  // The start of the line the region begins on, which bounds the backward
  // look for a property so a chain broken by a newline is not joined up.
  size_t lineStart = from;
  while (lineStart > 0 && utf8[lineStart - 1] != '\n') {
    lineStart--;
  }

  TokenClass open = TokenClass::Plain;
  uint32_t openStart = baseUtf16;
  uint32_t offset = baseUtf16;
  size_t spanStart = 0;
  uint32_t spanStartUtf16 = 0;
  bool inSpan = false;

  size_t index = from;
  while (index < to) {
    const int style = static_cast<unsigned char>(styles[index]) & kStyleMask;
    if (promote && style == identifier) {
      if (!inSpan) {
        inSpan = true;
        spanStart = index;
        spanStartUtf16 = offset;
      }
    } else if (inSpan) {
      inSpan = false;
      const TokenClass promoted = promotedClass(language, utf8, lineStart, spanStart, index);
      if (promoted != TokenClass::Plain) {
        fresh->push_back({spanStartUtf16, offset, promoted});
      }
    }

    const TokenClass token = classes[style];
    if (token != open) {
      if (open != TokenClass::Plain) {
        fresh->push_back({openStart, offset, open});
      }
      open = token;
      openStart = offset;
    }
    const Utf8Step step = utf8StepAt(utf8, index);
    offset += step.units;
    index += step.bytes;
  }
  if (inSpan) {
    const TokenClass promoted = promotedClass(language, utf8, lineStart, spanStart, index);
    if (promoted != TokenClass::Plain) {
      fresh->push_back({spanStartUtf16, offset, promoted});
    }
  }
  if (open != TokenClass::Plain) {
    fresh->push_back({openStart, offset, open});
  }
}

class LexillaHighlighter final : public CodeHighlighter {
public:
  void tokenize(std::string_view info, std::u16string_view text, HighlightState *state, std::vector<TokenRun> *runs,
                double budgetMs) override {
    invalidateLineage(state);
    tokenizeImpl(info, text, state, runs, budgetMs, false, false, false);
  }

  uint64_t lexersCreated() const override { return gLexersCreated.load(std::memory_order_relaxed); }

private:
  void tokenizeTrustedAppend(std::string_view info, std::u16string_view text, HighlightState *state,
                             std::vector<TokenRun> *runs, double budgetMs, bool live,
                             bool terminalLineComplete) override {
    tokenizeImpl(info, text, state, runs, budgetMs, true, live, terminalLineComplete);
  }
  void tokenizeTrustedDelta(std::string_view info, const SnapshotText &, std::u16string_view delta,
                            HighlightState *state, SnapshotVector<TokenRun> *runs, double budgetMs,
                            bool live, bool terminalLineComplete) override {
    tokenizeImpl(info, state->text, state, nullptr, budgetMs, true, live, terminalLineComplete, &delta, runs);
  }

  void tokenizeImpl(std::string_view info, std::u16string_view text, HighlightState *state,
                    std::vector<TokenRun> *runs, double budgetMs, bool trustedAppend, bool live,
                    bool terminalLineComplete, const std::u16string_view *delta = nullptr,
                    SnapshotVector<TokenRun> *publication = nullptr) {
    state->resumed = false;
    state->bytesLexed = state->runsBuilt = state->tokenBytesCopied = state->textBytesCopied = 0;
    const HighlightStop previousStop = state->lastStop;
    const LanguageTable *language = LanguageRegistry::find(info);
    // UTF-8 is never shorter than UTF-16, so an over-long text is over the
    // limit before any of it is converted.
    const size_t textLength = text.size() + (delta ? delta->size() : 0);
    if (language == nullptr || textLength == 0 || textLength > kMaxHighlightBytes) {
      state->clear();
      if (runs) runs->clear();
      if (publication) publication->clear();
      state->lastStop = HighlightStop::Skipped;
      return;
    }

    // The state is reused only when the block grew, so a stream pays for its
    // last line and nothing else.
    size_t from = 0;
    uint32_t baseUtf16 = 0;
    const bool joinsSurrogate = !state->text.empty() && state->text.size() < textLength &&
        state->text.back() >= 0xd800 && state->text.back() <= 0xdbff &&
        (delta ? delta->front() : text[state->text.size()]) >= 0xdc00 &&
        (delta ? delta->front() : text[state->text.size()]) <= 0xdfff;
    const bool appendApplies = trustedAppend && state->engine != nullptr &&
        state->language == language->alias && state->text.size() <= textLength && !joinsSurrogate;
    if (appendApplies || (!delta && state->applies(language->alias, text))) {
      state->resumed = true;
      from = state->finalEnd;
      baseUtf16 = state->finalUtf16;
      // Only the bytes the block gained are converted, so a pass over a long
      // streaming block costs its new tail rather than the whole thing.
      const std::u16string_view added = delta ? *delta : text.substr(state->text.size());
      const std::string bytes = utf16ToUtf8(added);
      state->document.append(bytes);
      state->text.append(added);
      state->textBytesCopied = added.size() * sizeof(char16_t) + bytes.size();
    } else {
      // A surrogate seam or language change requires a checked rebuild. Copy
      // the old state before clear(), since delta calls view that very state.
      std::u16string rebuilt;
      if (delta) {
        rebuilt.assign(text);
        rebuilt.append(*delta);
        text = rebuilt;
      }
      state->clear();
      state->language = language->alias;
      state->text.assign(text);
      state->document.resetOwned(utf16ToUtf8(text));
      state->textBytesCopied = text.size() * sizeof(char16_t);
      state->engine = createEngine(*language);
      if (state->engine == nullptr) {
        if (runs) runs->clear();
        if (publication) publication->clear();
        state->lastStop = HighlightStop::Skipped;
        return;
      }
    }
    if (state->document.length() > kMaxHighlightBytes) {
      state->clear();
      state->overByteLimit = true;
      if (runs) runs->clear();
      if (publication) publication->clear();
      state->lastStop = HighlightStop::Skipped;
      return;
    }

    const std::string_view utf8 = state->document.text();
    const size_t end = utf8.size();
    const size_t lineStart = state->document.unstableStart();
    const size_t lineLength = end - lineStart;
    const bool sameUnfinishedLine = state->resumed && lineStart == state->liveLineStart;
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
    const size_t styledEnd = lex(state->engine->lexer, &state->document, from, end, budgetMs, &state->bytesLexed);
    // Everything before the last line, or before wherever the budget stopped,
    // is complete: no later chunk can change a token there.
    const size_t stable = std::min(state->document.unstableStart(), styledEnd);

    // The last line is rebuilt every pass, so what the previous pass made of it
    // goes before the new runs are added.
    // The last retained run can merge with the rebuilt suffix, even when it
    // ends exactly at the stable boundary. Publish that run again as well.
    const size_t retainedRuns = state->resumed ? static_cast<size_t>(std::lower_bound(
        state->runs.begin(), state->runs.end(), baseUtf16,
        [](const TokenRun &run, uint32_t boundary) { return run.end < boundary; }) - state->runs.begin()) : 0;
    dropRunsFrom(&state->runs, baseUtf16);
    buildRuns(utf8, state->document, *language, state->engine->classes, from, styledEnd, baseUtf16, &scratch());
    appendRuns(&state->runs, scratch());
    state->runsBuilt = scratch().size();
    state->finalEnd = stable;
    state->finalUtf16 = baseUtf16 + utf16Length(utf8.substr(from, stable - from));
    // A block whose last line has no newline yet is fully colored but not yet
    // final; only bytes the pass never reached mean it ran out of budget.
    state->lastStop = styledEnd < end ? HighlightStop::Budget : HighlightStop::Complete;
    if (publication) {
      const TokenRun *suffix = state->runs.empty() ? nullptr : state->runs.data() + retainedRuns;
      state->tokenBytesCopied = publication->replaceSuffix(
          retainedRuns, suffix, state->runs.size() - retainedRuns).payloadBytes;
    } else {
      *runs = state->runs;
      state->tokenBytesCopied = state->runs.size() * sizeof(TokenRun);
    }
    state->liveLineStart = lineStart;
    state->nextLiveLineRefresh = std::max(kLiveLineRefreshThreshold, lineLength * 2);
  }
  /// Per thread, because one highlighter serves the whole worker pool and a
  /// pass builds its runs into this rather than allocating for a few entries.
  static std::vector<TokenRun> &scratch() {
    static thread_local std::vector<TokenRun> runs;
    return runs;
  }

  /// Lexes `[from, end)` in windows so a budget can be enforced between them,
  /// and returns how far it got.
  static size_t lex(ILexer5 *lexer, CodeDocument *document, size_t from, size_t end, double budgetMs,
                    uint64_t *bytesLexed) {
    auto view = codeDocumentView(*document);
    const auto started = std::chrono::steady_clock::now();
    size_t position = from;
    while (position < end) {
      const size_t line = document->lineIndexAt(position);
      const size_t window = std::min(end, document->lineStart(line + kWindowLines));
      if (window <= position) {
        break;
      }
      // Resuming at a line start needs only the style the previous line ended
      // in, which the document kept. A lexer that reaches further back reads
      // the same styles and line states an editor would have given it.
      const int initial = position > 0 ? static_cast<unsigned char>(document->styleAt(position - 1)) : 0;
      lexer->Lex(static_cast<Sci_PositionU>(position), static_cast<Sci_Position>(window - position), initial,
                 view.get());
      *bytesLexed += window - position;
      position = window;
      const double spent =
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
      if (position < end && spent >= budgetMs) {
        break;
      }
    }
    return position;
  }
};

} // namespace

HighlightEngine::~HighlightEngine() {
  if (lexer != nullptr) {
    lexer->Release();
  }
}

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
  // A previously unmatched high surrogate was encoded as U+FFFD. When the
  // append supplies its low surrogate, that byte prefix must be rebuilt.
  if (!text.empty() && text.size() < next.size() && text.back() >= 0xd800 && text.back() <= 0xdbff &&
      next[text.size()] >= 0xdc00 && next[text.size()] <= 0xdfff) {
    return false;
  }
  return engine != nullptr && language == alias && text.size() <= next.size() &&
      std::equal(text.begin(), text.end(), next.begin());
}

void HighlightState::clear() {
  sourceDocumentId_ = 0;
  resumed = false;
  lastStop = HighlightStop::Skipped;
  language.clear();
  text.clear();
  finalEnd = 0;
  finalUtf16 = 0;
  bytesLexed = runsBuilt = tokenBytesCopied = textBytesCopied = 0;
  nextLiveLineRefresh = liveLineStart = 0;
  overByteLimit = false;
  runs.clear();
  engine.reset();
  document.reset({});
}

void CodeHighlighter::tokenizeAppend(std::string_view info, const SnapshotText &text, std::u16string_view delta,
                                    HighlightState *state,
                                    SnapshotVector<TokenRun> *runs, double budgetMs, uint64_t previousDocumentId,
                                    size_t previousLength, uint64_t nextDocumentId, bool live,
                                    bool terminalLineComplete) {
  // Skipped blocks do not retain a lexer/text state. Recognize them before a
  // checked string-view conversion would flatten every growing snapshot.
  const bool retainedOversize = state->overByteLimit && previousDocumentId != 0 &&
      state->sourceDocumentId_ == previousDocumentId && previousLength + delta.size() == text.size();
  if (retainedOversize || LanguageRegistry::find(info) == nullptr || text.empty() || text.size() > kMaxHighlightBytes) {
    state->clear();
    state->overByteLimit = retainedOversize;
    runs->clear();
    state->sourceDocumentId_ = nextDocumentId;
    return;
  }
  const bool lineage = previousDocumentId != 0 && state->sourceDocumentId_ == previousDocumentId &&
      state->text.size() == previousLength && previousLength + delta.size() == text.size();
  invalidateLineage(state);
  if (lineage) {
    tokenizeTrustedDelta(info, text, delta, state, runs, budgetMs, live, terminalLineComplete);
  } else {
    std::vector<TokenRun> next;
    tokenize(info, text, state, &next, budgetMs);
    *runs = std::move(next);
  }
  state->sourceDocumentId_ = nextDocumentId;
}

CodeHighlighter &downstreamHighlighter() {
  static LexillaHighlighter highlighter;
  return highlighter;
}

} // namespace downstream
