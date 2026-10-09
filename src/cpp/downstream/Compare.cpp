#include "Compare.h"

namespace downstream::scan {

bool sameBlock(const Block &a, const Block &b) {
  // `info` and the meta it was parsed into both count. An identity promises that
  // it names the same content, and a block whose fence title changed is a block
  // whose header would draw a different label, so a meta-only change has to
  // give it a new identity or the committed-segment cache would draw the old
  // title forever.
  //
  // `math` and `images` count for the same reason. A formula's own layout and
  // an image's load state deliberately do not: a MathSpan's identity excludes
  // its layout so a block keeps its identity while a pass fills it in, and
  // `Image` records are the document's, compared in `sameDocument`.
  if (a.kind != b.kind || a.headingLevel != b.headingLevel || a.tableHeader != b.tableHeader ||
      a.table != b.table || a.spacingBefore != b.spacingBefore || a.text != b.text || a.language != b.language ||
      a.info != b.info || !(a.meta == b.meta) || a.runs.size() != b.runs.size() || a.cells.size() != b.cells.size() ||
      a.tokens.size() != b.tokens.size() || a.containers.size() != b.containers.size() ||
      a.math.size() != b.math.size() || a.images.size() != b.images.size() ||
      // The direction counts because it is part of what is drawn: a block whose
      // text is the same but whose lines break the other way is a different
      // block to a cached rendering, and the same reason `spacingBefore` does.
      a.direction != b.direction ||
      // A fence that has just been closed draws the same code, but it is no
      // longer the block a writer is typing into, so the caret moves and a
      // cached rendering with the caret inside the fence must not be reused.
      a.fenceOpen != b.fenceOpen) {
    return false;
  }
  for (size_t i = 0; i < a.runs.size(); i++) {
    const InlineRun &x = a.runs[i];
    const InlineRun &y = b.runs[i];
    if (x.start != y.start || x.end != y.end || x.styles != y.styles || x.link != y.link || x.tag != y.tag) {
      return false;
    }
  }
  for (size_t i = 0; i < a.tokens.size(); i++) {
    const TokenRun &x = a.tokens[i];
    const TokenRun &y = b.tokens[i];
    if (x.start != y.start || x.end != y.end || x.token != y.token) {
      return false;
    }
  }
  for (size_t i = 0; i < a.cells.size(); i++) {
    const TableCell &x = a.cells[i];
    const TableCell &y = b.cells[i];
    if (x.start != y.start || x.end != y.end || x.align != y.align) {
      return false;
    }
  }
  for (size_t i = 0; i < a.containers.size(); i++) {
    const Container &x = a.containers[i];
    const Container &y = b.containers[i];
    if (x.kind != y.kind || x.id != y.id || x.showsMarker != y.showsMarker || x.task != y.task ||
        x.marker != y.marker || x.open != y.open || x.tag != y.tag) {
      return false;
    }
  }
  for (size_t i = 0; i < a.math.size(); i++) {
    if (!(a.math[i] == b.math[i])) {
      return false;
    }
  }
  for (size_t i = 0; i < a.images.size(); i++) {
    if (a.images[i] != b.images[i]) {
      return false;
    }
  }
  return true;
}

bool sameTag(const Tag &a, const Tag &b) {
  return a.name == b.name && a.attributes == b.attributes && a.range == b.range &&
      a.block == b.block && a.bodyStart == b.bodyStart && a.bodyEnd == b.bodyEnd;
}

bool sameImage(const Image &a, const Image &b) {
  return a.url == b.url && a.alt == b.alt && a.title == b.title && a.loadable == b.loadable &&
      a.link == b.link && (a.range == b.range);
}

bool sameDocument(const Document &a, const Document &b) {
  if (a.blocks.size() != b.blocks.size() || a.links.size() != b.links.size() || a.images.size() != b.images.size() || a.tags.size() != b.tags.size()) {
    return false;
  }
  for (size_t i = 0; i < a.blocks.size(); i++) {
    if (!sameBlock(a.blocks[i], b.blocks[i])) {
      return false;
    }
  }
  for (size_t i = 0; i < a.links.size(); i++) {
    if (a.links[i].url != b.links[i].url || !(a.links[i].range == b.links[i].range) || a.links[i].tag != b.links[i].tag) {
      return false;
    }
  }
  for (size_t i = 0; i < a.images.size(); i++) {
    if (!sameImage(a.images[i], b.images[i])) {
      return false;
    }
  }
  for (size_t i = 0; i < a.tags.size(); i++) {
    if (!sameTag(a.tags[i], b.tags[i])) return false;
  }
  return true;
}

} // namespace downstream::scan
