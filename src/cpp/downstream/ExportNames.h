#pragma once

#include <string_view>

namespace downstream {

/// The file extension a fenced code block's language saves as, without the dot.
/// Empty when the language has no known extension, so a caller can fall back to
/// `txt`. The table comes from Streamdown's `languageExtensionMap`, vendored
/// with its Apache-2.0 notice in ExportNames.cpp.
std::string_view extensionForLanguage(std::string_view language);

} // namespace downstream
