#pragma once

#include "CodeLayout.h"

#include <cstdint>

namespace downstream {

/**
 * How one collapsible block lays out.
 *
 * The only member is whether the body is shown, which is the whole of it: a
 * collapsed body is a height question and nothing else, and it is answered by
 * the same choice that answers whether the summary reads as expanded.
 */
struct DetailsBlockLayout {
  /// True when the reader's answer and the writer's `open` agree, or when the
  /// reader has opened a block the writer left closed. A reader's choice always
  /// wins, because it is the later of the two answers.
  bool open{false};
};

/// The layout of the collapsible block at `ordinal`.
///
/// `writtenOpen` is the writer's `open` attribute. It is the starting state and
/// not a lock: a reader who closed a block the writer opened keeps it closed,
/// which is what makes the summary a control rather than a caption.
inline DetailsBlockLayout resolveDetails(const ReaderChoices &choices, uint32_t ordinal, bool writtenOpen) {
  return DetailsBlockLayout{hasChoice(choiceSet(choices, ChoiceKind::DetailsOpen), ordinal) || writtenOpen};
}

/**
 * The space a collapsible block's body takes, in points.
 *
 * A closed body is zero rather than its own height: the blocks are still in the
 * document, still in the copy and the export, and still measured by the text
 * layout that put them there — they are simply not stacked. Measuring them
 * anyway would reserve space a reader cannot reach, and skipping the text
 * layout would make a closed block's own height depend on whether it was open,
 * which is the property the cached segment plan relies on not having.
 */
inline float detailsBodyHeight(bool open, float bodyHeight) { return open ? bodyHeight : 0.0f; }

} // namespace downstream