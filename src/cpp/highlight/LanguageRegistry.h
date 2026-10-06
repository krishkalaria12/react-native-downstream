#pragma once

#include "Document.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace downstream {

/// A lexical post-pass a language asks for, over its identifier tokens.
enum MarkType : uint8_t {
  /// No post-pass. Languages whose lexers already classify calls, properties,
  /// and types, and the ones where the heuristics would misfire, ask for none.
  MarkNone = 0,
  /// An identifier followed by `(` becomes Function.
  MarkCalls = 1 << 0,
  /// An identifier after `.` becomes Property.
  MarkProperties = 1 << 1,
  /// An identifier that starts upper-case becomes Type.
  MarkTypes = 1 << 2,
};

/// One row of the generated language table in LanguageTables.cpp. The strings
/// live for the life of the process, so resolving a language allocates nothing.
struct LanguageTable {
  /// Lower-case info-string alias.
  const char *alias;
  /// Lexilla lexer name, for example "cpp".
  const char *lexer;
  /// Space-separated keyword lists in the lexer's own WordListSet order,
  /// terminated by an empty string.
  const char *const *words;
  /// Lexer property name and value pairs, terminated by an empty name.
  const char *const *properties;
  /// Lexer style number and token class, sorted by style number and
  /// terminated by a negative style. Styles with no entry are Plain.
  const int32_t *styles;
  /// A MarkType combination.
  uint8_t marks;
  /// The lexer style the post-pass may promote, or -1.
  int32_t identifier;
};

/// The generated table. Defined in LanguageTables.cpp.
const LanguageTable *languageTables(size_t *count);

/// Looks up the language for a fenced code block's info string.
///
/// The info string is trimmed and lower-cased, and only its first word is
/// considered, so ```TypeScript title="x" and ```js:src/app.js both resolve.
/// An unknown language returns null, which is the signal to render the block
/// plain.
class LanguageRegistry {
public:
  static const LanguageTable *find(std::string_view info);
  /// Number of registered aliases, for tests and dumps.
  static size_t count();
  /// The alias of the `index`th row, in table order.
  static std::string_view aliasAt(size_t index);
};

/// Blocks over this many UTF-8 bytes are not colored. The API contract in
/// The limit is fixed at 64 KiB.
constexpr size_t kMaxHighlightBytes = 64 * 1024;

/// Wall-clock budget for one highlighting pass, in milliseconds. A pass that
/// runs out leaves the rest of the block plain and resumes on the next one.
constexpr double kHighlightBudgetMs = 4.0;

} // namespace downstream
