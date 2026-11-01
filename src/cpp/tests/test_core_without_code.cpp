#include "CodeHighlighter.h"
#include "CodeLayout.h"
#include "Export.h"
#include "IncrementalParser.h"
#include "StreamLexer.h"
#include "tests/check.h"

using namespace downstream;
int main() {
  CHECK(!codeHighlighterAvailable(), "core works without a linked lexer package");
  CHECK(StreamLexer::created() == 0, "plain core creates no lexer");
  IncrementalParser parser;
  parser.append("```javascript\nconst answer = 42;\n```\n");
  const auto result = parser.parse(true);
  CHECK(result.status == ParseStatus::Ok, "plain fenced code parses");
  CHECK(result.document->blocks[0].tokens.empty(), "code stays plain without the optional package");
  CHECK(result.document->blocks[0].text == u"const answer = 42;", "code text remains available for layout and copy");
  CHECK(exportCodeText(*result.document, 0) == u"const answer = 42;", "plain code copies without fences or line numbers");
  CHECK(codeLineStarts(result.document->blocks[0].text) == CodeLineStarts{0}, "plain code retains its line-number positions");
  CHECK(gutterDigits(1, 1) == 2, "plain code retains the line-number gutter");
  CHECK(StreamLexer::created() == 0, "rendering code without the package creates no lexer");
  return ds_check_result();
}
