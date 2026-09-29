#include "LinkPolicy.h"

#include <cctype>

namespace downstream {

namespace {

bool startsWithIgnoringCase(std::string_view text, std::string_view prefix) {
  if (text.size() < prefix.size()) {
    return false;
  }
  for (size_t i = 0; i < prefix.size(); i++) {
    if (std::tolower(static_cast<unsigned char>(text[i])) != prefix[i]) {
      return false;
    }
  }
  return true;
}

} // namespace

bool isActionableUrl(std::string_view url) {
  for (const char c : url) {
    const auto byte = static_cast<unsigned char>(c);
    if (byte < 0x20 || byte == 0x7F) {
      return false;
    }
  }

  for (const std::string_view prefix : {std::string_view("https://"), std::string_view("http://")}) {
    if (startsWithIgnoringCase(url, prefix)) {
      return url.size() > prefix.size();
    }
  }
  if (startsWithIgnoringCase(url, "mailto:")) {
    return url.size() > 7;
  }
  return false;
}

} // namespace downstream
