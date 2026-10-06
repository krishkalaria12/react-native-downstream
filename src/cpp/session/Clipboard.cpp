#include "Clipboard.h"

#include <atomic>

namespace downstream {

namespace {

/// The installed writer, or null. An atomic rather than a mutex because it is
/// written once at install and read from the JavaScript thread forever after,
/// and a lock on that path would be a lock on every copy a reader ever asks for.
std::atomic<ClipboardWriter> writer{nullptr};

} // namespace

void installClipboardWriter(ClipboardWriter next) {
  ClipboardWriter expected = nullptr;
  writer.compare_exchange_strong(expected, next);
}

bool clipboardAvailable() {
  return writer.load(std::memory_order_acquire) != nullptr;
}

bool copyTextToClipboard(std::string_view text) {
  const ClipboardWriter write = writer.load(std::memory_order_acquire);
  return write != nullptr && write(text);
}

} // namespace downstream