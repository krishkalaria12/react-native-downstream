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

bool isLoadableImageUrl(std::string_view url) {
  // A cap here rather than at the loader: a URL long enough to be an attack is
  // one the document should never have held, and refusing it at the parse means
  // no view, no request and no cache entry is ever made for it.
  if (url.empty() || url.size() > kMaxImageUrlBytes) {
    return false;
  }
  // A space is not a control character, but a loader that receives one has to
  // guess whether it is part of the URL or a separator, and either guess is
  // attackable, so it disqualifies the URL here.
  for (const char c : url) {
    const auto byte = static_cast<unsigned char>(c);
    if (byte <= 0x20 || byte == 0x7F) {
      return false;
    }
  }

  constexpr std::string_view scheme = "https://";
  if (!startsWithIgnoringCase(url, scheme)) {
    return false;
  }
  const std::string_view rest = url.substr(scheme.size());
  // A host has to be there, and it has to end before any path, query, or
  // fragment, or the "host" is a userinfo section a reader would never see.
  size_t hostLength = rest.find_first_of("/?#");
  if (hostLength == std::string_view::npos) {
    hostLength = rest.size();
  }
  const std::string_view host = rest.substr(0, hostLength);
  if (host.empty() || host.find('@') != std::string_view::npos) {
    return false;
  }
  return true;
}

} // namespace downstream
