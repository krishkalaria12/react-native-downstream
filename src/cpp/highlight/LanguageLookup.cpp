#include "Languages.h"

#include <algorithm>
#include <cstring>

namespace downstream {
namespace {

constexpr std::string_view kBlanks = " \t\r\n";

/// Longer than any alias, so a word past it cannot match and is not copied.
constexpr size_t kMaxAliasLength = 32;

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

const Language *findLanguage(std::string_view info) {
  const std::string_view word = languageWord(info);
  if (word.empty() || word.size() > kMaxAliasLength) {
    return nullptr;
  }
  char lowered[kMaxAliasLength];
  std::transform(word.begin(), word.end(), lowered, lowerAscii);
  const std::string_view key(lowered, word.size());
  const Language *end = kLanguages + kLanguageCount;
  const Language *found = std::lower_bound(
      kLanguages, end, key, [](const Language &row, std::string_view value) { return std::string_view(row.alias) < value; });
  return found != end && std::string_view(found->alias) == key ? found : nullptr;
}

} // namespace downstream
