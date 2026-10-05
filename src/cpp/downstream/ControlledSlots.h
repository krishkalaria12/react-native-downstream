#pragma once

#include "FenceSlots.h"
#include "MarkdownParser.h"

#include <cstddef>
#include <string>
#include <vector>

namespace downstream {

/// The size of the source a controlled query parses on the calling thread.
///
/// The same bound layout waits for (`kWaitForPreparedBytes`), which is what
/// makes this the same wait moved earlier rather than a new one: a source
/// larger than this goes to the worker pool, as it already did for layout.
inline constexpr size_t kSyncSlotBytes = 16 * 1024;

/// The answer to a controlled query: the slots when the source could be read
/// here and now, and `pending` when it was too large to parse on this thread
/// and the caller has to ask again through `parsedFenceSlots` on a worker.
struct ControlledSlots {
  bool pending{false};
  std::vector<FenceSlot> slots;
};

/// The fence slots a controlled source holds, answered from a finished
/// preparation when one exists and otherwise from a bare parse up to
/// `kSyncSlotBytes`. Live is true: a controlled string is the whole message, so
/// a fence the writer never closed reads as incomplete until the app settles it,
/// which is Streamdown's rule for a streaming string that has not finished.
///
/// Answers are cached by the source, the parse flags *and* the registered
/// languages, so a chat that re-renders the same message, or a StrictMode double
/// render, parses nothing twice — and an app that registers a renderer later
/// reads a different answer rather than the one a renderer-free screen made.
/// A cached answer and a fresh one are always the same answer.
ControlledSlots controlledFenceSlots(const std::string &source, ParseOptions parse,
                                     const std::vector<std::string> &languages);

/// The same answer for a source of any size, parsed here without a bound or a
/// cache: what a worker runs once a controlled query has said `pending`.
std::vector<FenceSlot> parsedFenceSlots(const std::string &source, ParseOptions parse,
                                        const std::vector<std::string> &languages);

/// Drops cached answers. For tests.
void clearControlledSlotCache();

} // namespace downstream
