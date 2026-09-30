// Host tests for the syntax highlighter: the language table, the token offsets,
// the limits, the budget, and the guarantee that a grown block ends up with the
// tokens a one-shot pass gives it.

#include "CodeBlocks.h"
#include "CodeHighlighter.h"
#include "LanguageRegistry.h"
#include "MarkdownParser.h"
#include "Unicode.h"
#include "tests/check.h"

#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace downstream;

namespace {

std::vector<TokenRun> tokenize(std::string_view info, std::string_view code, double budgetMs = kHighlightBudgetMs) {
  HighlightState state;
  std::vector<TokenRun> runs;
  lexillaHighlighter().tokenize(info, utf8ToUtf16(code), &state, &runs, budgetMs);
  return runs;
}

/// Grows `code` `chunk` UTF-16 units at a time and returns the final runs.
std::vector<TokenRun> stream(std::string_view info, const std::u16string &code, size_t chunk) {
  HighlightState state;
  std::vector<TokenRun> runs;
  for (size_t at = 0; at < code.size(); at += chunk) {
    const size_t take = chunk < code.size() - at ? chunk : code.size() - at;
    lexillaHighlighter().tokenize(info, code.substr(0, at + take), &state, &runs, kHighlightBudgetMs);
  }
  return runs;
}

bool sameTokens(const std::vector<TokenRun> &a, const std::vector<TokenRun> &b) {
  if (a.size() != b.size()) {
    return false;
  }
  for (size_t index = 0; index < a.size(); index++) {
    if (a[index].start != b[index].start || a[index].end != b[index].end || a[index].token != b[index].token) {
      return false;
    }
  }
  return true;
}

/// Every run must lie inside the block and sit on a UTF-16 boundary, and no two
/// may overlap. A platform attribute built from a run that breaks these rules
/// would color the wrong characters or crash.
bool validAgainst(const std::vector<TokenRun> &runs, const std::u16string &text) {
  uint32_t previous = 0;
  for (const TokenRun &run : runs) {
    if (run.start >= run.end || run.end > text.size() || run.token == TokenClass::Plain) {
      return false;
    }
    if (run.start < previous) {
      return false;
    }
    if (run.start > 0 && run.start < text.size() && text[run.start - 1] >= 0xd800 && text[run.start - 1] <= 0xdbff) {
      return false;
    }
    if (run.end < text.size() && text[run.end] >= 0xdc00 && text[run.end] <= 0xdfff) {
      return false;
    }
    previous = run.end;
  }
  return true;
}

void testRegistry() {
  CHECK(LanguageRegistry::count() > 40, "the table covers the languages chat models emit");
  // An info string is trimmed, lower-cased, cut at the first space, and cut at
  // a colon, so the shapes a fence actually takes all resolve.
  CHECK(LanguageRegistry::find("ts") != nullptr, "ts resolves");
  CHECK(LanguageRegistry::find("TypeScript") != nullptr, "a mixed-case alias resolves");
  CHECK(LanguageRegistry::find("  TS  ") != nullptr, "a padded alias resolves");
  CHECK(LanguageRegistry::find("ts title=\"a.ts\"") != nullptr, "a meta string is ignored");
  CHECK(LanguageRegistry::find("js:src/app.js") != nullptr, "a colon suffix is ignored");
  CHECK(LanguageRegistry::find("C++") != nullptr, "a punctuation alias resolves");
  CHECK(LanguageRegistry::find("") == nullptr, "no language resolves to plain");
  CHECK(LanguageRegistry::find("   ") == nullptr, "a blank info string resolves to plain");
  CHECK(LanguageRegistry::find("brainfuck") == nullptr, "an unknown language resolves to plain");
  CHECK(LanguageRegistry::find("typescriptx") == nullptr, "a longer unknown word resolves to plain");

  // Every row must resolve by its own alias and carry the tables the highlighter
  // reads, or a language would silently render plain.
  for (size_t index = 0; index < LanguageRegistry::count(); index++) {
    const std::string_view alias = LanguageRegistry::aliasAt(index);
    const LanguageTable *language = LanguageRegistry::find(alias);
    CHECK(language != nullptr, "every table row resolves by its own alias");
    CHECK(language->lexer != nullptr && language->lexer[0] != '\0', "every language names a lexer");
    CHECK(language->styles != nullptr && language->styles[0] >= 0, "every language has a style map");
    CHECK(language->words != nullptr, "every language has a word list table");
  }
}

void testTokens() {
  const std::string typescript = "const x: number = 1;\n";
  const std::vector<TokenRun> runs = tokenize("ts", typescript);
  CHECK(!runs.empty(), "a TypeScript block has tokens");
  const std::u16string text = utf8ToUtf16(typescript);
  CHECK(validAgainst(runs, text), "runs lie inside the block on code point boundaries");
  // Adjacent runs of one class are merged, so the list is what a platform walks
  // once per color change.
  for (size_t index = 1; index < runs.size(); index++) {
    CHECK(!(runs[index].start == runs[index - 1].end && runs[index].token == runs[index - 1].token),
          "adjacent runs of one class are merged");
  }

  // The post-pass is what tells a call, a property, and a type apart. A call
  // wins over a property, which wins over a type, and Function and Type share a
  // color anyway, so the visible difference is a call against a plain access.
  const std::string calls = "loadUser(id);\nres.status;\nconst value: User = x;\nobj.method();\n";
  const std::vector<TokenRun> marked = tokenize("ts", calls);
  bool function = false;
  bool property = false;
  bool type = false;
  for (const TokenRun &run : marked) {
    if (run.token == TokenClass::Function) {
      function = true;
    }
    if (run.token == TokenClass::Property) {
      property = true;
    }
    if (run.token == TokenClass::Type) {
      type = true;
    }
  }
  CHECK(function, "an identifier before ( becomes a function");
  CHECK(property, "an identifier after . becomes a property");
  CHECK(type, "an upper-case identifier becomes a type");
  // A method call after a dot is a call, not a property access, so it takes the
  // function color rather than the property one.
  const std::u16string callsText = utf8ToUtf16(calls);
  for (const TokenRun &run : marked) {
    if (run.token == TokenClass::Function) {
      CHECK(callsText.substr(run.start, run.end - run.start) != u"status",
            "a method call is a function rather than a property");
    }
  }

  // Python's own lexer tells a definition name from a call, so the post-pass
  // has nothing to add there.
  const std::vector<TokenRun> python = tokenize("py", "def greet(name):\n    return name\n");
  bool sawDefinition = false;
  for (const TokenRun &run : python) {
    if (run.token == TokenClass::Function) {
      sawDefinition = true;
    }
  }
  CHECK(sawDefinition, "the Python lexer classifies a definition name as a function");
}

void testBoundaries() {
  CHECK(tokenize("brainfuck", "x = 1;\n").empty(), "an unknown language has no tokens");
  CHECK(tokenize("", "x = 1;\n").empty(), "a fence with no language has no tokens");

  // The contract in docs/api.md skips a block over 64 KiB of UTF-8.
  std::string atLimit;
  while (atLimit.size() + sizeof("const a = 1;\n") - 1 <= kMaxHighlightBytes) {
    atLimit += "const a = 1;\n";
  }
  atLimit += std::string(kMaxHighlightBytes - atLimit.size(), 'x');
  CHECK(atLimit.size() == kMaxHighlightBytes, "the sample is exactly at the limit");
  CHECK(!tokenize("ts", atLimit).empty(), "a block at the limit is colored");
  CHECK(tokenize("ts", atLimit + "x").empty(), "a block one byte over the limit is plain");

  // Emoji, CJK, RTL text, and CRLF all have to survive the offset mapping.
  const std::string mixed = "const a = \"\xF0\x9F\x91\x8D\";\nconst \xD7\xA9 = \"\xE6\xBC\xA2\xE5\xAD\x97\";\r\n// \xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D\n";
  const std::vector<TokenRun> runs = tokenize("ts", mixed);
  CHECK(validAgainst(runs, utf8ToUtf16(mixed)), "emoji, CJK, and RTL keep their offsets");
  bool sawKeyword = false;
  for (const TokenRun &run : runs) {
    if (run.token == TokenClass::Keyword) {
      sawKeyword = true;
    }
  }
  CHECK(sawKeyword, "a block with wide characters still colors its keywords");

  // A state left over from another block must not color the next one.
  HighlightState state;
  std::vector<TokenRun> out;
  lexillaHighlighter().tokenize("ts", utf8ToUtf16("const a = 1;\n"), &state, &out, kHighlightBudgetMs);
  const size_t before = out.size();
  CHECK(before > 0, "the first block colored");
  lexillaHighlighter().tokenize("brainfuck", utf8ToUtf16("const a = 1;\n"), &state, &out, kHighlightBudgetMs);
  CHECK(out.empty(), "an unknown language clears the previous block's tokens");
  lexillaHighlighter().tokenize("ts", utf8ToUtf16("const b"), &state, &out, kHighlightBudgetMs);
  CHECK(!out.empty(), "the same language starts a new block rather than resuming");
  // A shorter text is not a prefix, so it starts over too.
  lexillaHighlighter().tokenize("ts", utf8ToUtf16("const b"), &state, &out, kHighlightBudgetMs);
  CHECK(state.resumed, "an unchanged text resumes");
  lexillaHighlighter().tokenize("ts", utf8ToUtf16("const bc"), &state, &out, kHighlightBudgetMs);
  CHECK(state.resumed, "a grown text resumes");
  lexillaHighlighter().tokenize("ts", utf8ToUtf16("const bd"), &state, &out, kHighlightBudgetMs);
  CHECK(!state.resumed, "a same-length rewrite starts over");
  lexillaHighlighter().tokenize("ts", utf8ToUtf16("const bd\n"), &state, &out, kHighlightBudgetMs);
  CHECK(state.resumed, "a completed line still resumes");
  lexillaHighlighter().tokenize("ts", utf8ToUtf16("other\n"), &state, &out, kHighlightBudgetMs);
  CHECK(!state.resumed, "text that is not an extension starts over");
}

void testBudget() {
  // A budget of zero must not color the whole block in one pass but must still
  // make progress on later passes, which is what stops a long block from
  // staying plain forever. Highlighting works through windows of lines, so the
  // block below needs more passes than the test gives it.
  std::string block;
  while (block.size() < 24 * 1024) {
    block += "const value = 1;\n";
  }
  const std::u16string text = utf8ToUtf16(block);
  HighlightState state;
  std::vector<TokenRun> runs;
  lexillaHighlighter().tokenize("ts", text, &state, &runs, 0.0);
  CHECK(state.lastStop == HighlightStop::Budget, "a zero budget stops the pass early");
  const size_t firstPass = state.finalEnd;
  CHECK(firstPass > 0, "a stopped pass still colors its first window");
  CHECK(firstPass < text.size(), "a stopped pass leaves the rest of the block plain");
  for (int pass = 0; pass < 40; pass++) {
    lexillaHighlighter().tokenize("ts", text, &state, &runs, 0.0);
  }
  CHECK(state.finalEnd > firstPass, "later passes make progress");
  CHECK(state.lastStop != HighlightStop::Complete, "a block this long does not finish within 40 passes");
  // With no budget the whole block colors, and the runs are the same as a
  // one-shot pass's.
  HighlightState whole;
  std::vector<TokenRun> wholeRuns;
  lexillaHighlighter().tokenize("ts", text, &whole, &wholeRuns, 1e9);
  CHECK(whole.lastStop == HighlightStop::Complete, "an unbounded pass colors the whole block");
  CHECK(sameTokens(wholeRuns, tokenize("ts", block, 1e9)), "a resumed block equals a one-shot pass");
}

struct StreamingCase {
  const char *id;
  const char *info;
  const char *code;
};

// Every case has something a resumed pass could get wrong: a construct that
// spans lines, one that spans a chunk boundary, or a preprocessor region whose
// state a fresh lexer would lose.
const StreamingCase streamingCases[] = {
    {"ts-call", "ts", "export async function loadUser(id: number): Promise<User | null> {\n  const res = await fetch(`/api/users/${id}`);\n  return res;\n}\n"},
    {"ts-preprocessor", "ts", "#if defined(A) && A > 1\nint x = 1;\n#else\nint y = 2;\n#endif\nint z = 3;\n"},
    {"ts-template", "ts", "const t = `line ${a}\ncontinued ${b}`;\nconst u = 1;\n"},
    {"py-fstring", "py", "def greet(name):\n    print(f\"hi {name}\")\n    return None\n"},
    {"py-triple", "py", "s = '''\nmulti\nline\n'''\nprint(s)\n"},
    {"go-raw", "go", "s := `raw\nstring`\nt := 1\n"},
    {"c-block-comment", "c", "int main(void) {\n  /* block\n     comment */\n  return 0;\n}\n"},
    {"html-nested", "html", "<div class=\"a\">\n  <p>text</p>\n</div>\n"},
    {"json-array", "json", "{\n  \"a\": [1, 2],\n  \"b\": null\n}\n"},
    {"sh-loop", "sh", "for f in *.txt; do\n  echo \"$f\"\ndone\n"},
    {"rb-interp", "rb", "def hi(x)\n  puts \"#{x}\"\nend\n"},
    {"md-fence", "md", "# Title\n\n*em* and `code`\n\n- item\n"},
    {"diff-hunk", "diff", "--- a\n+++ b\n@@ -1 +1 @@\n-a\n+b\n"},
    {"yaml-nested", "yaml", "a: 1\nb:\n  - c\n"},
    {"ts-wide", "ts", "const \xF0\x9F\x91\x8D = \"\xE6\xBC\xA2\xE5\xAD\x97\";\nconst \xD7\xA9 = 1;\n"},
    {"unknown", "brainfuck", "+++++[>++++<-].\n"},
};

void testStreaming() {
  for (const StreamingCase &testCase : streamingCases) {
    const std::u16string text = utf8ToUtf16(testCase.code);
    const std::vector<TokenRun> want = tokenize(testCase.info, testCase.code, 1e9);
    CHECK(validAgainst(want, text), testCase.id);
    for (size_t chunk : {size_t(1), size_t(3), size_t(7), size_t(40), size_t(512)}) {
      const std::vector<TokenRun> got = stream(testCase.info, text, chunk);
      CHECK(sameTokens(want, got), testCase.id);
    }
  }
}

void testIsolation() {
  // A document with no code block must never create a lexer, which is what
  // keeps prose off the highlighting path entirely.
  const uint64_t before = lexillaHighlighter().lexersCreated();
  const ParseResult prose = parseMarkdown("# Title\n\nSome prose with `inline code` and a [link](https://x).\n");
  CHECK(prose.status == ParseStatus::Ok, "the prose sample parses");
  for (const Block &block : prose.document->blocks) {
    CHECK(block.tokens.empty(), "prose blocks carry no tokens");
  }
  CHECK(lexillaHighlighter().lexersCreated() == before, "prose creates no lexer");

  // A parse does not color: the controlled path and the stream path each do it
  // on their own job, so a parse stays free of lexer work.
  const ParseResult code = parseMarkdown("```ts\nconst a = 1;\n```\n");
  CHECK(code.status == ParseStatus::Ok, "the code sample parses");
  CHECK(lexillaHighlighter().lexersCreated() == before, "a parse creates no lexer");
  CHECK(code.document->blocks[0].tokens.empty(), "a parse carries no tokens");
  const DocumentHandle colored = highlightCodeBlocks(lexillaHighlighter(), code.document, kHighlightBudgetMs, nullptr);
  CHECK(lexillaHighlighter().lexersCreated() == before + 1, "coloring a fenced block creates one lexer");
  CHECK(!colored->blocks[0].tokens.empty(), "coloring fills the block's tokens");
}

void testDocuments() {
  // The controlled path: highlighting reaches the document a parse produced.
  const ParseResult parsed = parseMarkdown("```ts\nconst a = 1;\n```\n");
  const DocumentHandle colored = highlightCodeBlocks(lexillaHighlighter(), parsed.document, kHighlightBudgetMs, nullptr);
  CHECK(!colored->blocks[0].tokens.empty(), "coloring a document fills its code block");
  CHECK(parsed.document->blocks[0].tokens.empty(), "coloring leaves the parse result it was given alone");
  // Coloring the same text twice must give the same runs, or a committed
  // snapshot and a fresh parse of it would differ.
  const DocumentHandle again = highlightCodeBlocks(lexillaHighlighter(), parsed.document, kHighlightBudgetMs, nullptr);
  CHECK(again->blocks[0].tokens.size() == colored->blocks[0].tokens.size(), "coloring is deterministic");

  // A document with no code block is returned as it stands, so a prose-only
  // source shares one prepared result instead of copying it.
  const ParseResult plain = parseMarkdown("Just prose.\n");
  CHECK(highlightCodeBlocks(lexillaHighlighter(), plain.document, kHighlightBudgetMs, nullptr).get() == plain.document.get(),
        "a prose document is not copied");

  // An indented code block has no language, so it stays plain.
  const ParseResult indented = parseMarkdown("    const a = 1;\n");
  CHECK(indented.document->blocks[0].kind == BlockKind::CodeBlock, "an indented block is code");
  CHECK(indented.document->blocks[0].tokens.empty(), "an indented block has no language and stays plain");

  // Blocks are colored in a mutable list, which is the shape the streaming
  // parser builds a committed chunk in.
  std::vector<Block> blocks(parsed.document->blocks.begin(), parsed.document->blocks.end());
  HighlightStats stats;
  highlightCodeBlocks(lexillaHighlighter(), &blocks, kHighlightBudgetMs, &stats);
  CHECK(stats.blocks == 1, "one code block is counted");
  CHECK(!blocks[0].tokens.empty(), "the mutable list is colored");
}

/// The controlled path colors on the shared worker pool, so two documents can
/// be colored at once. Each thread has its own block and its own resume state,
/// and they must not see each other's tokens.
void testConcurrentPasses() {
  const std::vector<std::pair<std::string, std::string>> work = {
      {"ts", "export const a = 1;\n"},   {"py", "def f():\n    return 1\n"},
      {"sh", "echo hi\n"},              {"json", "{\"a\": 1}\n"},
      {"html", "<p>hi</p>\n"},           {"css", ".a { color: red; }\n"},
      {"sql", "SELECT 1;\n"},            {"rs", "fn f() {}\n"},
  };
  // The expected tokens come from a single threaded pass, so a thread that saw
  // another thread's scratch state would disagree with this.
  std::vector<std::vector<TokenRun>> expected;
  expected.reserve(work.size());
  for (const auto &entry : work) {
    expected.push_back(tokenize(entry.first, entry.second, 1e9));
  }

  std::vector<std::vector<TokenRun>> results(work.size());
  std::vector<std::string> failures;
  std::mutex failureMutex;
  std::vector<std::thread> threads;
  for (size_t index = 0; index < work.size(); index++) {
    threads.emplace_back([&, index] {
      const std::string &info = work[index].first;
      const std::u16string code = utf8ToUtf16(work[index].second);
      HighlightState state;
      std::vector<TokenRun> runs;
      for (int repeat = 0; repeat < 8; repeat++) {
        lexillaHighlighter().tokenize(info, code, &state, &runs, kHighlightBudgetMs);
      }
      results[index] = runs;
      if (!sameTokens(expected[index], runs)) {
        std::lock_guard<std::mutex> lock(failureMutex);
        failures.push_back(info);
      }
    });
  }
  for (std::thread &thread : threads) {
    thread.join();
  }
  CHECK(failures.empty(), "concurrent passes do not see each other's tokens");
  for (size_t index = 0; index < work.size(); index++) {
    CHECK(!results[index].empty(), "a concurrent pass colors its block");
  }
}

} // namespace

int main() {
  testRegistry();
  testTokens();
  testBoundaries();
  testBudget();
  testStreaming();
  testIsolation();
  testDocuments();
  testConcurrentPasses();
  return ds_check_result();
}
