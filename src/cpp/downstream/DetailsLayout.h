#pragma once

#include "CodeLayout.h"
#include "Document.h"

#include <cstdint>
#include <utility>
#include <vector>

namespace downstream {

/**
 * How one collapsible block lays out.
 *
 * The only member is whether the body is shown, which is the whole of it: a
 * collapsed body is a height question and nothing else, and it is answered by the
 * same choice that answers whether the summary reads as expanded.
 */
struct DetailsBlockLayout {
  /// Whether the body is showing: the writer's `open` with the reader's own answer
  /// applied on top. A reader's answer always wins, because it is the later of the
  /// two.
  bool open{false};
};

/**
 * The layout of the collapsible block a container describes.
 *
 * `writtenOpen` is the writer's `open` attribute, read from the container rather
 * than from a block, because it is a property of the block pair and both of the
 * blocks inside it have to agree.
 *
 * The key is the container's own id rather than a separate ordinal. It is already
 * unique, already counted in source order, already stable across append-only
 * snapshots, and already on every container both platforms draw from — a fifth
 * counter that had to be offset the same way `ParseResult::containerCount` is
 * would be a second way to say the same thing.
 */
inline DetailsBlockLayout resolveDetails(const ReaderChoices &choices, uint32_t containerId, bool writtenOpen) {
  // A recorded choice *flips* the writer's state rather than only being able to
  // open it, which is what lets a reader close a block the writer left open. With
  // no record the writer decides, so the default is the document the app sent.
  return DetailsBlockLayout{hasChoice(choiceSet(choices, ChoiceKind::DetailsOpen), containerId) != writtenOpen};
}

/// True when this block sits inside a `<details>` whose body is not shown.
///
/// A summary is exempt from its own marked frame. Any closed ancestor is
/// enough: a block inside two collapsible blocks, the outer of
/// which is closed, is not visible however open the inner one is.
inline bool insideHiddenDetails(const std::vector<Container> &containers, const ReaderChoices &choices) {
  for (const Container &container : containers) {
    if (container.kind == ContainerKind::Details && !container.showsMarker &&
        !resolveDetails(choices, container.id, container.open).open) {
      return true;
    }
  }
  return false;
}

/// True when this block is a `<details>`'s summary: the container's own first
/// leaf, which is where the chevron and the pressed state are drawn.
///
/// The innermost frame with the marker, because the marker is consumed by the
/// first leaf of *its* container: a nested summary is inside the outer one, so
/// asking the outer frame would put two chevrons on the outer line.
inline bool isDetailsSummary(const std::vector<Container> &containers) {
  for (const Container &container : containers) {
    if (container.kind != ContainerKind::Details) {
      continue;
    }
    if (container.showsMarker) {
      return true;
    }
  }
  return false;
}

/**
 * The space a collapsible block's body takes, in points.
 *
 * A closed body is zero rather than its own height: the blocks are still in the
 * document, still in the copy and the export, and still measured by the text
 * layout that put them there — they are simply not stacked.
 */
inline float detailsBodyHeight(bool open, float bodyHeight) { return open ? bodyHeight : 0.0f; }

/**
 * One collapsed run of a text segment: the character range of a block whose
 * `<details>` body is closed.
 *
 * A range rather than a block index because the platform lays the segment out as
 * one text layout and its lines are what are hidden, not its blocks.
 */
struct HiddenRange {
  uint32_t start{0};
  uint32_t end{0};
};

/**
 * The height a text segment shows, from the whole layout's height and the top of
 * each of its lines.
 *
 * A line is hidden when its first character lies inside a collapsed range, which
 * is the whole of the rule: a block's own lines are what a closed body occupies,
 * and the line that starts it is the one that goes with it. The lines come from
 * the *whole* layout rather than from a second layout of the visible text, so the
 * visible text wraps exactly as it would if the body were open — which is what
 * makes opening a body a pure height change and nothing else.
 *
 * `lineStarts[i]` is the character offset line `i` begins at and `lineTops[i]` its
 * top, so there is one more entry than lines.
 */
inline float visibleTextHeight(float totalHeight, const std::vector<float> &lineTops,
                               const std::vector<uint32_t> &lineStarts, const std::vector<HiddenRange> &hidden) {
  if (hidden.empty() || lineTops.size() < 2 || lineStarts.empty()) {
    return totalHeight;
  }
  const size_t lines = std::min(lineTops.size(), lineStarts.size() + 1) - 1;
  float removed = 0;
  for (size_t line = 0; line < lines; line++) {
    const uint32_t at = lineStarts[line];
    for (const HiddenRange &range : hidden) {
      if (at >= range.start && at < range.end) {
        removed += lineTops[line + 1] - lineTops[line];
        break;
      }
    }
  }
  return totalHeight - removed;
}

} // namespace downstream