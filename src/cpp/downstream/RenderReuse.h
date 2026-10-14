#pragma once

#include "Compare.h"
#include "SegmentPlan.h"

namespace downstream {

inline bool sameRenderTag(const Document &previous, const Document &next, int32_t index) {
  return index < 0 || (static_cast<size_t>(index) < previous.tags.size() &&
      static_cast<size_t>(index) < next.tags.size() && scan::sameTag(previous.tags[index], next.tags[index]));
}

inline bool sameRenderLink(const Document &previous, const Document &next, int32_t index) {
  if (index < 0) return true;
  if (static_cast<size_t>(index) >= previous.links.size() || static_cast<size_t>(index) >= next.links.size()) return false;
  const auto &a = previous.links[index];
  const auto &b = next.links[index];
  return a.url == b.url && a.range == b.range && a.tag == b.tag && sameRenderTag(previous, next, a.tag);
}

/// Block identities also key native caches. Equal local reference indexes or
/// TeX alone cannot preserve an identity when resolved content changes.
inline bool sameRenderBlock(const Document &previous, const Block &a, const Document &next, const Block &b) {
  if (!scan::sameBlock(a, b) || a.highlightComplete != b.highlightComplete || a.fenceMarker != b.fenceMarker ||
      a.fenceLength != b.fenceLength || a.fenceClosed != b.fenceClosed) return false;
  for (const auto &run : a.runs) {
    if (!sameRenderLink(previous, next, run.link) || !sameRenderTag(previous, next, run.tag)) return false;
  }
  for (const auto &container : a.containers) {
    if (!sameRenderTag(previous, next, container.tag)) return false;
  }
  for (uint32_t image : a.images) {
    if (image >= previous.images.size() || image >= next.images.size() ||
        !scan::sameImage(previous.images[image], next.images[image]) ||
        !sameRenderLink(previous, next, previous.images[image].link)) return false;
  }
  for (size_t formula = 0; formula < a.math.size(); ++formula) {
    if (a.math[formula].layout != b.math[formula].layout || a.math[formula].error != b.math[formula].error) return false;
  }
  return true;
}

/// Finish-only payload proof. Settlement and streaming are view state, while
/// every render input, resolved referent, formula answer and segment boundary
/// must still match. No hashes or provisional block identities are involved.
inline bool canReuseRendering(const Document &previous, const Document &next) {
  if (!scan::sameDocument(previous, next)) return false;
  for (size_t index = 0; index < next.blocks.size(); ++index) {
    const Block &a = previous.blocks[index];
    const Block &b = next.blocks[index];
    if (!sameRenderBlock(previous, a, next, b)) return false;
  }
  const auto before = planSegments(previous);
  const auto after = planSegments(next);
  if (before.size() != after.size()) return false;
  for (size_t index = 0; index < before.size(); ++index) {
    const auto &a = before[index];
    const auto &b = after[index];
    if (a.from != b.from || a.to != b.to || a.kind != b.kind || a.codeOrdinal != b.codeOrdinal ||
        a.tableOrdinal != b.tableOrdinal || a.imageOrdinal != b.imageOrdinal ||
        a.customContainer != b.customContainer) return false;
  }
  return true;
}

} // namespace downstream
