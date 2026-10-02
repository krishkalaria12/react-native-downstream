#pragma once

// Saying a formula out loud.
//
// KaTeX's answer is hidden MathML and relies on a screen reader to interpret
// it, which no native API has and which KaTeX's own issue #820 records as
// unreliable on the web. So this is a small table walk over the TeX rather than
// a second parse: a reader who navigates by rotor or who has VoiceOver read a
// formula should hear "x equals negative b plus or minus the square root of
// delta, over two a", and the source is always there as the fallback.
//
// It is deliberately a table and not an engine. A verbalizer that had to be
// correct about every construct would be a second implementation of TeX, and a
// wrong one would read a formula aloud incorrectly rather than not at all —
// which is the worse failure. An unknown command is spoken as its own name
// without the backslash, so an unsupported construct is intelligible instead of
// silent, and the reader can switch to the source.

#include <string>
#include <string_view>

namespace downstream::math {

/// The spoken form of one formula, as UTF-8.
///
/// Never empty for a non-empty formula: an unrecognized formula becomes its
/// source, which is what the caller falls back to anyway, so this can only
/// improve on the fallback and never lose to it.
std::string speak(std::string_view tex, bool display);

/// One command's spoken name, or an empty string for a command with no name of
/// its own. Exposed for a test that checks the table rather than the walk.
std::string_view commandName(std::string_view command);

} // namespace downstream::math
