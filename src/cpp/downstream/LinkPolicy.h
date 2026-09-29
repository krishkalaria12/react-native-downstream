#pragma once

#include <string_view>

namespace downstream {

/// The built-in link policy: only absolute http:, https:, and mailto: targets
/// without control characters are actionable. Scheme matching ignores case.
/// Everything else renders as plain text and never emits a link event.
bool isActionableUrl(std::string_view url);

} // namespace downstream
