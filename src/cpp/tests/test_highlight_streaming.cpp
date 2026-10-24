#include "CodeBlocks.h"
#include "IncrementalParser.h"
#include "Unicode.h"
#include "tests/check.h"

#include <string>
#include <vector>

using namespace downstream;

namespace {

bool sameTokens(const std::vector<TokenRun> &a, const std::vector<TokenRun> &b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); i++) {
    if (a[i].start != b[i].start || a[i].end != b[i].end || a[i].token != b[i].token) return false;
  }
  return true;
}

bool validTokens(const Block &block) {
  uint32_t previous = 0;
  for (const TokenRun &run : block.tokens) {
    if (run.start < previous || run.start >= run.end || run.end > block.text.size()) return false;
    if (run.start > 0 && block.text[run.start - 1] >= 0xd800 && block.text[run.start - 1] <= 0xdbff) return false;
    if (run.end < block.text.size() && block.text[run.end] >= 0xdc00 && block.text[run.end] <= 0xdfff) return false;
    previous = run.end;
  }
  return true;
}

void checkComplete(const Block &block) {
  HighlightState state;
  std::vector<TokenRun> expected;
  downstreamHighlighter().tokenize(block.language, block.text, &state, &expected, 1e9);
  CHECK(sameTokens(block.tokens, expected), "a complete live refresh matches the checked highlighter");
}

void testLongLine() {
  std::string code = "const value = ";
  while (code.size() < 60 * 1024) code += "compute(user.id, 42) + ";
  IncrementalParser parser(ParseMode::IncrementalTail);
  parser.append("```ts\n");
  parser.parse(false);
  DocumentHandle retained;
  std::u16string retainedText;
  std::vector<TokenRun> retainedTokens;
  uint64_t contiguousBytes = 0;
  const TokenRun *previousTokenData = nullptr;
  for (size_t at = 0; at < code.size(); at += 16) {
    parser.append(std::string_view(code).substr(at, 16));
    const auto result = parser.parse(false);
    CHECK(result.status == ParseStatus::Ok, "long line parses");
    const Block &block = result.document->blocks.back();
    CHECK(validTokens(block), "complete and provisional tokens stay inside the current text");
    if (block.highlightComplete) checkComplete(block);
    else {
      contiguousBytes += block.text.flattenedBytes();
      CHECK(block.tokens.data() == previousTokenData, "deferred snapshots share the previous immutable token publication");
    }
    previousTokenData = block.tokens.data();
    if (!block.highlightComplete && !retained) {
      retained = result.document;
      retainedText = block.text;
      retainedTokens = block.tokens;
    }
  }
  const auto counters = parser.counters();
  CHECK(counters.highlightCount(HighlightStop::Deferred) > 1000, "long line snapshots defer repeated lexing");
  CHECK(counters.highlightBytesLexed < 1024 * 1024, "long line lexer input stays bounded across tiny chunks");
  CHECK(counters.fenceSnapshotStorageBytesCopied < 12 * 1024 * 1024,
        "live fence snapshot page copies stay bounded across tiny chunks");
  CHECK(counters.fenceAppendScanBytes <= code.size(), "raw fence newline scans visit appended bytes only");
  CHECK(contiguousBytes == 0, "trusted deferred parser snapshots do not flatten their growing text");
  CHECK(retained && retained->blocks.back().text == retainedText &&
        sameTokens(retained->blocks.back().tokens, retainedTokens), "retained snapshots keep text and provisional tokens");
  parser.append("\n");
  const auto newline = parser.parse(false);
  CHECK(newline.document->blocks.back().highlightComplete, "source newline forces refresh even though md4c strips it");
  checkComplete(newline.document->blocks.back());
  parser.append("const after = 1;");
  parser.parse(false);
  const auto finished = parser.parse(true);
  CHECK(finished.document->blocks.back().highlightComplete, "finished open fence refreshes its tail");
  checkComplete(finished.document->blocks.back());

  parser.replace("```py\nprint('reset')");
  const auto replacement = parser.parse(false);
  CHECK(replacement.document->blocks.back().highlightComplete, "reset and language replacement clear cadence state");
  checkComplete(replacement.document->blocks.back());
  parser.reset();
  CHECK(parser.counters().highlightCount(HighlightStop::Deferred) == 0, "reset clears work counters");
}

void testSkippedStorage() {
  for (const std::string language : {"", "unknown-language", "ts"}) {
    IncrementalParser parser(ParseMode::IncrementalTail);
    // The known-language fixture starts beyond the highlight size cap.
    parser.append("```" + language + "\n" + std::string(language == "ts" ? 70000 : 128, 'x'));
    parser.parse(false);
    for (size_t i = 0; i < 100; i++) {
      parser.append("another plain append ");
      const Block &block = parser.parse(false).document->blocks.back();
      CHECK(block.tokens.empty(), "unknown, absent and oversized languages keep plain tokens");
      CHECK(block.text.flattenedBytes() == 0, "skipped live code snapshots never flatten for a discarded lexer state");
    }
  }
  IncrementalParser wide(ParseMode::IncrementalTail);
  std::string source = "```ts\n";
  for (size_t i = 0; i < 25000; i++) source += "漢";
  wide.append(source);
  wide.parse(false);
  // The first append establishes exact snapshot lineage after the full parse.
  wide.append("x");
  wide.parse(false);
  for (size_t i = 0; i < 100; i++) {
    wide.append("more");
    const Block &block = wide.parse(false).document->blocks.back();
    CHECK(block.tokens.empty() && block.text.flattenedBytes() == 0,
          "UTF-8 oversized text below the UTF-16 cap stays skipped without flattening");
  }
}

void testUnicodeAndClose() {
  IncrementalParser parser(ParseMode::IncrementalTail);
  parser.append("```ts\nconst emoji = '");
  parser.parse(false);
  const std::string code = std::string(5000, 'a') + "😀漢字';";
  for (size_t at = 0; at < code.size(); at++) {
    parser.append(std::string_view(code).substr(at, 1));
    const auto result = parser.parse(false);
    CHECK(validTokens(result.document->blocks.back()), "byte-split Unicode never splits UTF-16 token boundaries");
  }
  parser.append("\n```\n\nparagraph\n");
  const auto closed = parser.parse(true);
  CHECK(closed.document->blocks[0].highlightComplete, "closed fence finishes deferred highlighting");
  checkComplete(closed.document->blocks[0]);
}

void testFinishWhileDeferred() {
  IncrementalParser parser(ParseMode::IncrementalTail);
  parser.append("```ts\nconst text = '" + std::string(5000, 'x'));
  parser.parse(false);
  // The first append associates the checked highlighting state with the
  // snapshot lineage. The following append can use the live cadence.
  parser.append("more");
  parser.parse(false);
  parser.append("';");
  const auto live = parser.parse(false);
  CHECK(!live.document->blocks.back().highlightComplete, "paused long line keeps provisional highlighting");
  const auto final = parser.parse(true);
  CHECK(final.document->blocks.back().highlightComplete, "finish forces a deferred line without a source newline");
  checkComplete(final.document->blocks.back());
}

void testCheckedCallAlwaysRefreshes() {
  HighlightState state;
  std::vector<TokenRun> runs;
  std::u16string text = u"const value = '";
  text.append(6000, u'x');
  downstreamHighlighter().tokenize("ts", text, &state, &runs, 1e9);
  text.append(u"';");
  downstreamHighlighter().tokenize("ts", text, &state, &runs, 1e9);
  CHECK(state.lastStop == HighlightStop::Complete && state.bytesLexed == state.document.length(),
        "arbitrary checked calls always relex an unfinished line");
  text.push_back(0xd83d);
  downstreamHighlighter().tokenize("ts", text, &state, &runs, 1e9);
  text.push_back(0xde00);
  downstreamHighlighter().tokenize("ts", text, &state, &runs, 1e9);
  HighlightState whole;
  std::vector<TokenRun> expected;
  downstreamHighlighter().tokenize("ts", text, &whole, &expected, 1e9);
  CHECK(!state.resumed && sameTokens(runs, expected), "a split surrogate rebuilds the UTF-8 prefix");
}


void testManyCompletedLines() {
  IncrementalParser parser(ParseMode::IncrementalTail);
  parser.append("```ts\n");
  parser.parse(false);
  std::vector<DocumentHandle> retained;
  for (size_t line = 0; line < 5200; line++) {
    parser.append("let x=1;\n");
    const auto result = parser.parse(false);
    const Block &block = result.document->blocks.back();
    CHECK(block.highlightComplete, "each completed tiny line has fresh tokens");
    CHECK(block.tokens.flattenedBytes() == 0, "live tiny-line token publication never flattens its stable prefix");
    if (line % 101 == 0) {
      retained.push_back(result.document);
      checkComplete(block);
    }
  }
  const auto counters = parser.counters();
  CHECK(counters.highlightTokenBytesCopied < 5 * 1024 * 1024,
        "over five thousand tiny lines copy token pages rather than growing full vectors");
  for (const auto &snapshot : retained) {
    CHECK(validTokens(snapshot->blocks.back()), "retained completed-line tokens keep their original offsets");
    checkComplete(snapshot->blocks.back());
  }
  const auto finished = parser.parse(true);
  checkComplete(finished.document->blocks.back());

  // Strings/comments may join a token across a completed line. The token
  // touching the stable boundary belongs to the dirty suffix, not its prefix.
  for (const std::string code : {"/*first\nsecond\nthird*/", "`first\nsecond\nthird`"}) {
    parser.replace("```ts\n");
    parser.parse(false);
    DocumentHandle first;
    std::vector<TokenRun> firstTokens;
    for (const char unit : code) {
      parser.append(std::string_view(&unit, 1));
      const auto result = parser.parse(false);
      if (!first && unit == '\n') {
        first = result.document;
        firstTokens = first->blocks.back().tokens;
      }
      if (result.document->blocks.back().highlightComplete) checkComplete(result.document->blocks.back());
    }
    checkComplete(parser.parse(true).document->blocks.back());
    CHECK(first && sameTokens(first->blocks.back().tokens, firstTokens),
          "retained tokens crossing line joins remain immutable after suffix merge");
  }
}

} // namespace

int main() {
  testManyCompletedLines();
  testSkippedStorage();
  testLongLine();
  testUnicodeAndClose();
  testFinishWhileDeferred();
  testCheckedCallAlwaysRefreshes();
  return ds_check_result();
}
