#include "Caret.h"

namespace downstream {

CaretPlace placeCaret(const Document &document, const CaretSpec &spec) {
  if (spec.kind == CaretKind::None) {
    return CaretPlace::None;
  }
  const bool showing = spec.animating == Animating::On ||
      (spec.animating == Animating::Auto && document.streaming);
  if (!showing) {
    return CaretPlace::None;
  }
  if (document.blocks.empty()) {
    // Nothing has arrived yet, so the caret is the whole document: it takes a
    // line of its own where the first paragraph will go.
    return CaretPlace::OwnLine;
  }

  const Block &last = document.blocks.back();
  switch (last.kind) {
    case BlockKind::Paragraph:
    case BlockKind::Heading:
      // Prose. The next character arrives on this line, so the caret follows the
      // text rather than sitting alone below it.
      return CaretPlace::AfterText;
    case BlockKind::CodeBlock:
      // An open fence is where code is being written, so the caret belongs
      // inside it. Streamdown hides it there for CSS reasons; natively it is the
      // cursor at the end of the block.
      return last.fenceOpen ? CaretPlace::AfterText : CaretPlace::OwnLine;
    default:
      break;
  }
  // A list item is a leaf block with a container, whatever its own kind is.
  for (const Container &container : last.containers) {
    if (container.kind == ContainerKind::ListItem) {
      return CaretPlace::AfterText;
    }
  }
  // A table, a rule, an HTML block, or a closed fence: the next block will start
  // on a fresh line, so the caret takes that line rather than sitting at the end
  // of a finished one.
  return CaretPlace::OwnLine;
}

} // namespace downstream