#pragma once

#include <string_view>

namespace downstream {

/**
 * Writes `text` to the platform clipboard, and reports whether it did.
 *
 * The signature is the one a platform can satisfy without this library knowing
 * anything about either: iOS hands `UIPasteboard` a string, Android hands its
 * clipboard manager one, and neither needs a runtime or a thread.
 */
using ClipboardWriter = bool (*)(std::string_view text);

/**
 * Installs the platform's clipboard, once per process.
 *
 * A seam rather than a platform `#ifdef` because `src/cpp/session` is compiled
 * as plain C++ on both platforms and neither UIKit nor the Android clipboard
 * manager is reachable from it. The module that already exists on each platform
 * installs this, so the TurboModule that answers `copyText` has a writer by the
 * time JavaScript can ask.
 *
 * Installing twice keeps the first writer: the copy a reader just asked for is
 * already on its way, and swapping the writer underneath it would put two
 * different implementations on one request.
 */
void installClipboardWriter(ClipboardWriter writer);

/** Whether a writer is installed. */
bool clipboardAvailable();

/**
 * Copies `text`, or reports that nothing could.
 *
 * False rather than an exception: a clipboard is a convenience, and a reader
 * whose platform has none still gets the confirmation and the Open button.
 */
bool copyTextToClipboard(std::string_view text);

} // namespace downstream