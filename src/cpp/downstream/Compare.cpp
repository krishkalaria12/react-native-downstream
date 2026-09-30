#include "Compare.h"

namespace downstream::scan {

bool sameBlock(const Block &a, const Block &b) {
  if (a.kind != b.kind || a.headingLevel != b.headingLevel || a.tableHeader != b.tableHeader ||
      a.table != b.table || a.spacingBefore != b.spacingBefore || a.text != b.text || a.language != b.language ||
      a.runs.size() != b.runs.size() || a.cells.size() != b.cells.size() ||
      a.tokens.size() != b.tokens.size() ||
      a.containers.size() != b.containers.size()) {
    return false;
  }
  for (size_t i = 0; i < a.runs.size(); i++) {
    const InlineRun &x = a.runs[i];
    const InlineRun &y = b.runs[i];
    if (x.start != y.start || x.end != y.end || x.styles != y.styles || x.link != y.link) {
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
        x.marker != y.marker) {
      return false;
    }
  }
  return true;
}

bool sameDocument(const Document &a, const Document &b) {
  if (a.blocks.size() != b.blocks.size() || a.links.size() != b.links.size()) {
    return false;
  }
  for (size_t i = 0; i < a.blocks.size(); i++) {
    if (!sameBlock(a.blocks[i], b.blocks[i])) {
      return false;
    }
  }
  for (size_t i = 0; i < a.links.size(); i++) {
    if (a.links[i].url != b.links[i].url || !(a.links[i].range == b.links[i].range)) {
      return false;
    }
  }
  return true;
}

} // namespace downstream::scan
