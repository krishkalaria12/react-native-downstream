#include "CodeLayout.h"

namespace downstream {

CodeBlockLayout resolveCodeBlock(const CodeLayout &layout, uint32_t ordinal, const FenceMeta &meta) {
  CodeBlockLayout block;
  // A toggled block is the only one that differs from the prop, which is what
  // makes the decision one membership test rather than a per-block rule.
  block.wraps = hasChoice(layout.choices.wrapToggles, ordinal) != layout.props.wrap;
  // The prop wins over the meta, so `lineNumbers={false}` hides numbers even
  // where a fence asked for them.
  block.lineNumbers = layout.props.lineNumbers && !meta.noLineNumbers;
  block.firstNumber = meta.startLine;
  // An expanded block has no cap, and so has an uncapped one: both reach the
  // card as the single "no cap" value.
  block.maxHeight = hasChoice(layout.choices.expanded, ordinal) ? 0.0f : layout.props.maxHeight;
  return block;
}

CodeLineStarts codeLineStarts(std::u16string_view text) {
  std::vector<uint32_t> starts;
  starts.reserve(static_cast<size_t>(std::min<size_t>(text.size(), 4096) / 24 + 1));
  // Always one entry, so an empty fence has a first line to number and a draw
  // pass has a line to find.
  starts.push_back(0);
  for (uint32_t index = 0; index < text.size(); index++) {
    if (text[index] == u'\n') {
      starts.push_back(index + 1);
    }
  }
  return starts;
}

} // namespace downstream
