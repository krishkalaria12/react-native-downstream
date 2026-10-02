#include "downstream/MathScan.h"
#include "tests/check.h"

#include <string>

using downstream::scan::rewriteMathDelimiters;

int main() {
  for (const std::string source : {"Plain prose with (parentheses) and [links].",
                                   "Use `one` and ``two`` in prose.",
                                   "```ts\nconst value = `template`;\n```\n",
                                   "Ordinary \\commands with no bracket math."}) {
    const auto result = rewriteMathDelimiters(source, true);
    CHECK(result.text.empty() && result.pairs.empty(), "non-math source needs no rewritten copy");
    CHECK(result.original == source, "the original source stays available");
  }
  const std::string pair = "Before \\(x+y\\) and \\[a+b\\] after.";
  const auto result = rewriteMathDelimiters(pair, true);
  CHECK(result.text == "Before $$x+y$$ and $$a+b$$ after.", "bracket pairs rewrite at the same offsets");
  CHECK(result.text.size() == pair.size() && result.pairs.size() == 2, "rewriting preserves source length");
  CHECK(result.pairs.size() == 2 && !result.pairs[0].display && result.pairs[1].display,
        "paren and bracket display flags stay distinct");
  CHECK(rewriteMathDelimiters(pair, false).text.empty(), "disabled bracket math makes no copy");

  for (const std::string source : {"`\\(x\\)` and ``\\[y\\]``",
                                   "```\n\\(x\\) `inner`\n```\n",
                                   "~~~\n\\[x\\]\n~~~\n",
                                   "\\\\(x\\)", "An unpaired \\(x", "An unpaired \\[x"}) {
    CHECK(rewriteMathDelimiters(source, true).pairs.empty(), "code, escapes and incomplete pairs stay literal");
  }

  const std::string mixed = "~~~\n\\(code\\)\n~~~\nText \\(first\\).\n\n"
                            "`\\(inline code\\)` with \\[second\\].\n\n"
                            "~~~\n\\[more code\\]\n~~~\nThen \\(third\\).";
  const auto selected = rewriteMathDelimiters(mixed, true);
  CHECK(selected.pairs.size() == 3, "ordered code regions skip only code");
  CHECK(selected.pairs.size() == 3 && selected.pairs[0].open == mixed.find("\\(first") &&
        selected.pairs[1].open == mixed.find("\\[second") && selected.pairs[2].open == mixed.find("\\(third"),
        "formulas after and between code regions keep their source offsets");

  std::string many;
  for (int i = 0; i < 2000; i++) {
    many += "Use `code` and ``more`` with \\(x+y\\).\n\n";
  }
  const auto repeated = rewriteMathDelimiters(many, true);
  CHECK(repeated.pairs.size() == 2000, "many code spans never hide later math");
  CHECK(repeated.text.size() == many.size(), "large mixed documents preserve offsets");

  const auto fence = rewriteMathDelimiters("\\[\nx+y\n\\]\n", true);
  CHECK(fence.pairs.size() == 1 && fence.pairs.front().fenceLine, "standalone display delimiters stay fences");
  return ds_check_result();
}
