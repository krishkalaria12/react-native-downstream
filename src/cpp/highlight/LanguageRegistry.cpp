#include "LanguageRegistry.h"

namespace downstream {
namespace {

constexpr std::string_view kBlanks = " \t\r\n";

char lowerAscii(char value) {
  return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
}

/// The first word of an info string, trimmed and without a colon suffix.
std::string_view languageWord(std::string_view info) {
  const size_t first = info.find_first_not_of(kBlanks);
  if (first == std::string_view::npos) {
    return {};
  }
  const size_t end = info.find_first_of(kBlanks, first);
  info = info.substr(first, end == std::string_view::npos ? std::string_view::npos : end - first);
  // A fenced info string may carry a path after the language, as in
  // ```js:src/app.js, which names the same language.
  return info.substr(0, info.find(':'));
}

} // namespace

const LanguageTable *LanguageRegistry::find(std::string_view info) {
  const std::string_view word = languageWord(info);
  if (word.empty()) {
    return nullptr;
  }
  size_t count = 0;
  const LanguageTable *rows = languageTables(&count);
  for (size_t index = 0; index < count; index++) {
    const std::string_view alias = rows[index].alias;
    if (alias.size() != word.size()) {
      continue;
    }
    size_t at = 0;
    while (at < alias.size() && lowerAscii(word[at]) == alias[at]) {
      at++;
    }
    if (at == alias.size()) {
      return &rows[index];
    }
  }
  return nullptr;
}

size_t LanguageRegistry::count() {
  size_t count = 0;
  languageTables(&count);
  return count;
}

std::string_view LanguageRegistry::aliasAt(size_t index) {
  size_t count = 0;
  const LanguageTable *rows = languageTables(&count);
  return index < count ? rows[index].alias : std::string_view();
}

} // namespace downstream
