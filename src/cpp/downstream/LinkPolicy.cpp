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

/// Whether `url` is `scheme:` followed by something. The scheme's own grammar
/// is not checked: a scheme this policy does not know is refused further down,
/// and one whose body is empty is not a target of anything.
bool hasScheme(std::string_view url, std::string_view prefix, size_t *bodyStart) {
  if (!startsWithIgnoringCase(url, prefix)) {
    return false;
  }
  *bodyStart = prefix.size();
  return url.size() > *bodyStart;
}

/// `tel:` is followed by `+` or a digit. A scheme that accepts anything after it
/// would let `tel:` alone — or `tel:<script>` — reach the platform's handler as
/// a number-shaped string, and the two rules above it are the whole point of
/// narrowing the scheme list at all.
bool isTelTarget(std::string_view body) {
  return body[0] == '+' || (body[0] >= '0' && body[0] <= '9');
}

} // namespace

bool isRelativeLink(std::string_view url) {
  if (url.empty()) {
    return false;
  }
  if (url[0] == '#' || url[0] == '/') {
    return true;
  }
  if (url[0] == '?') {
    return true;
  }
  // `./` and `../`, and only with the separator: a lone `.` is a sentence and a
  // `..` is an ellipsis.
  if (url[0] != '.') {
    return false;
  }
  if (url.size() > 1 && url[1] == '/') {
    return true;
  }
  return url.size() > 2 && url[1] == '.' && url[2] == '/';
}

bool isActionableUrl(std::string_view url, bool allowRelative) {
  for (const char c : url) {
    const auto byte = static_cast<unsigned char>(c);
    if (byte < 0x20 || byte == 0x7F) {
      return false;
    }
  }

  size_t body = 0;
  for (const std::string_view prefix : {std::string_view("https://"), std::string_view("http://")}) {
    if (hasScheme(url, prefix, &body)) {
      return true;
    }
  }
  if (hasScheme(url, "mailto:", &body)) {
    return true;
  }
  if (hasScheme(url, "tel:", &body)) {
    return isTelTarget(url.substr(body));
  }
  // Streamdown's set, and the reason they are here rather than left out: an app
  // that has these handlers installed expects a chat client to follow them, and
  // every one of them takes a name rather than a page, so the same
  // reader-controlled-string argument that keeps images HTTPS-only does not
  // apply.
  for (const std::string_view prefix :
       {std::string_view("irc:"), std::string_view("ircs:"), std::string_view("xmpp:")}) {
    if (hasScheme(url, prefix, &body)) {
      return true;
    }
  }
  // A relative reference is opt-in because it has no meaning without a base:
  // `docs/a` and `/docs/a` are the same words, and only the second says which
  // document it means once the app has resolved it.
  return allowRelative && isRelativeLink(url);
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