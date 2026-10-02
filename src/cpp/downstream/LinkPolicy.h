#pragma once

#include <cstddef>
#include <string_view>

namespace downstream {

/// The longest URL an image may have, in bytes. Longer than any real asset and
/// shorter than a source the engine would refuse anyway, so the limit bounds
/// what reaches a loader rather than what a reader can write.
inline constexpr size_t kMaxImageUrlBytes = 8 * 1024;

/// The built-in link policy: only absolute http:, https:, and mailto: targets
/// without control characters are actionable. Scheme matching ignores case.
/// Everything else renders as plain text and never emits a link event.
bool isActionableUrl(std::string_view url);

/// The built-in image policy: only an absolute https: URL is fetched.
///
/// Stricter than the link policy in one way and looser in another. No http:,
/// because a model that can be made to write a URL can be made to write a
/// plaintext one, and a chat list that fetches them leaks its reader's presence
/// to whoever wrote the text. No `data:`, `file:`, or relative URL: the first
/// two are not images either platform's pipeline takes from a string, and a
/// relative URL has no base to resolve against. A host is required, and a
/// control character or space anywhere disqualifies the URL, because a loader
/// that has to sanitize what it was given is a loader that can be made to fetch
/// something else.
///
/// What this does not decide is whether the URL is one *this app* should fetch.
/// That is `allowedImagePrefixes`, which is a view prop rather than a parse
/// rule, so the same document parses the same way under two policies.
bool isLoadableImageUrl(std::string_view url);

} // namespace downstream
