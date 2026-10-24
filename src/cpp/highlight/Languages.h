#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace downstream {

/// One row of the generated alias table in Languages.cpp. The strings live for
/// the life of the process, so resolving a language allocates nothing.
struct Language {
  /// Lower-case info-string alias.
  const char *alias;
  /// The lexer module's language number for it.
  uint8_t id;
};

/// The generated table, sorted by alias. Defined in Languages.cpp.
extern const Language kLanguages[];
extern const size_t kLanguageCount;

/// Looks up the language for a fenced code block's info string.
///
/// The info string is trimmed and lower-cased, and only its first word is
/// considered, so ```TypeScript title="x" and ```js:src/app.js both resolve.
/// An unknown language returns null, which is the signal to render the block
/// plain. So does plain text, which has nothing to color.
const Language *findLanguage(std::string_view info);

/// Blocks over this many UTF-8 bytes are not colored. The limit is fixed at
/// 64 KiB.
constexpr size_t kMaxHighlightBytes = 64 * 1024;

/// Wall-clock budget for one highlighting pass, in milliseconds. A pass that
/// runs out leaves the rest of the block plain and resumes on the next one.
constexpr double kHighlightBudgetMs = 4.0;

} // namespace downstream
