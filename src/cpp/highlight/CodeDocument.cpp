#include "CodeDocument.h"

#include "ILexer.h"

#include <algorithm>
#include <cstring>

namespace downstream {
namespace {

/// UTF-8 sequence length from a lead byte, or 1 for anything invalid. Matches
/// what Lexilla's own test document does, so offsets and the lexers agree.
unsigned char sequenceLength(unsigned char lead) {
  if (lead < 0x80) {
    return 1;
  }
  if ((lead & 0xe0) == 0xc0) {
    return 2;
  }
  if ((lead & 0xf0) == 0xe0) {
    return 3;
  }
  if ((lead & 0xf8) == 0xf0) {
    return 4;
  }
  return 1;
}

/// The `IDocument` view of a CodeDocument. Kept in this file so the renamed
/// Scintilla namespace never reaches a header outside the highlighter.
class LexDocument final : public Scintilla::IDocument {
public:
  explicit LexDocument(CodeDocument &document) : document_(document) {}

  int SCI_METHOD Version() const override { return Scintilla::dvRelease4; }
  void SCI_METHOD SetErrorStatus(int) override {}
  Sci_Position SCI_METHOD Length() const override { return document_.length(); }

  void SCI_METHOD GetCharRange(char *buffer, Sci_Position position, Sci_Position count) const override {
    const std::string &text = document_.text();
    if (position < 0 || count <= 0) {
      return;
    }
    const size_t start = std::min<size_t>(static_cast<size_t>(position), text.size());
    const size_t available = text.size() - start;
    const size_t take = std::min<size_t>(static_cast<size_t>(count), available);
    if (take > 0) {
      std::memcpy(buffer, text.data() + start, take);
    }
    // Callers read a NUL-terminated copy; a short read leaves the rest zeroed.
    if (take < static_cast<size_t>(count)) {
      std::memset(buffer + take, 0, static_cast<size_t>(count) - take);
    }
  }

  char SCI_METHOD StyleAt(Sci_Position position) const override {
    return position < 0 ? 0 : document_.styleAt(static_cast<size_t>(position));
  }

  Sci_Position SCI_METHOD LineFromPosition(Sci_Position position) const override {
    if (position >= Length()) {
      return document_.lineCount() > 0 ? static_cast<Sci_Position>(document_.lineCount() - 1) : 0;
    }
    return static_cast<Sci_Position>(document_.lineIndexAt(static_cast<size_t>(position)));
  }

  Sci_Position SCI_METHOD LineStart(Sci_Position line) const override {
    if (line <= 0) {
      return 0;
    }
    return document_.lineStart(static_cast<size_t>(line));
  }

  // Folding levels are unused by every vendored lexer, so one level for all.
  int SCI_METHOD GetLevel(Sci_Position) const override { return 0x400; }
  int SCI_METHOD SetLevel(Sci_Position, int) override { return 0x400; }

  int SCI_METHOD GetLineState(Sci_Position line) const override {
    return line < 0 ? 0 : document_.lineState(static_cast<size_t>(line));
  }

  int SCI_METHOD SetLineState(Sci_Position line, int state) override {
    if (line < 0) {
      return 0;
    }
    return document_.setLineState(static_cast<size_t>(line), state);
  }

  void SCI_METHOD StartStyling(Sci_Position position) override {
    endStyled_ = position < 0 ? 0 : static_cast<size_t>(position);
  }

  bool SCI_METHOD SetStyleFor(Sci_Position count, char style) override {
    return fillStyle(endStyled_, count, style);
  }

  bool SCI_METHOD SetStyles(Sci_Position count, const char *styles) override {
    return fill(endStyled_, count, styles);
  }

  void SCI_METHOD DecorationSetCurrentIndicator(int) override {}
  void SCI_METHOD DecorationFillRange(Sci_Position, int, Sci_Position) override {}
  // Lexers call this when state they cannot resume from changed, as Scintilla
  // does. Nothing watches a code block, and the caller's own next snapshot
  // re-lexes from a line start regardless.
  void SCI_METHOD ChangeLexerState(Sci_Position, Sci_Position) override {}

  int SCI_METHOD CodePage() const override { return 65001; }
  bool SCI_METHOD IsDBCSLeadByte(char) const override { return false; }
  const char *SCI_METHOD BufferPointer() override { return document_.text().c_str(); }
  int SCI_METHOD GetLineIndentation(Sci_Position) override { return 0; }

  Sci_Position SCI_METHOD LineEnd(Sci_Position line) const override {
    const size_t count = document_.lineCount();
    const size_t index = static_cast<size_t>(std::max<Sci_Position>(line, 0));
    if (index + 1 >= count) {
      return Length();
    }
    size_t end = document_.lineStart(index + 1);
    if (end > 0) {
      end--; // Back over the newline.
      if (end > document_.lineStart(index) && document_.text()[end - 1] == '\r') {
        end--;
      }
    }
    return static_cast<Sci_Position>(end);
  }

  Sci_Position SCI_METHOD GetRelativePosition(Sci_Position start, Sci_Position offset) const override {
    const std::string &text = document_.text();
    Sci_Position position = std::clamp<Sci_Position>(start, 0, Length());
    while (offset < 0) {
      if (position <= 0) {
        return -1;
      }
      const unsigned char previous = static_cast<unsigned char>(text[static_cast<size_t>(position) - 1]);
      if (previous < 0x80) {
        position--;
        offset++;
      } else {
        while (position > 0 && (static_cast<unsigned char>(text[static_cast<size_t>(position) - 1]) & 0xc0) == 0x80) {
          position--;
        }
        position--;
        offset++;
      }
    }
    while (offset > 0 && position < Length()) {
      position += sequenceLength(static_cast<unsigned char>(text[static_cast<size_t>(position)]));
      offset--;
    }
    return position;
  }

  int SCI_METHOD GetCharacterAndWidth(Sci_Position position, Sci_Position *width) const override {
    const std::string &text = document_.text();
    if (position < 0 || position >= Length()) {
      if (width != nullptr) {
        *width = 1;
      }
      return '\0';
    }
    const size_t index = static_cast<size_t>(position);
    const unsigned char lead = static_cast<unsigned char>(text[index]);
    if (lead < 0x80) {
      if (width != nullptr) {
        *width = 1;
      }
      return lead;
    }
    const size_t count = std::min<size_t>(sequenceLength(lead), text.size() - index);
    if (width != nullptr) {
      *width = static_cast<Sci_Position>(count);
    }
    int value = lead & (0xff >> (count + 1));
    for (size_t i = 1; i < count; i++) {
      value = (value << 6) | (static_cast<unsigned char>(text[index + i]) & 0x3f);
    }
    return value;
  }

private:
  bool fill(size_t start, Sci_Position count, const char *bytes) {
    const size_t take = span(start, count);
    if (take == 0) {
      return false;
    }
    document_.writeStyles(start, bytes, take);
    endStyled_ = start + take;
    return true;
  }

  /// One style byte for `count` bytes, which is what a lexer sends when its
  /// run is longer than the accessor's buffer.
  bool fillStyle(size_t start, Sci_Position count, char style) {
    const size_t take = span(start, count);
    if (take == 0) {
      return false;
    }
    document_.fillStyles(start, take, style);
    endStyled_ = start + take;
    return true;
  }

  /// How much of a styling run fits in the document, never more than asked for
  /// and never past the end.
  size_t span(size_t start, Sci_Position count) const {
    if (count <= 0 || start >= document_.length()) {
      return 0;
    }
    return std::min<size_t>(static_cast<size_t>(count), document_.length() - start);
  }

  CodeDocument &document_;
  size_t endStyled_{0};
};

} // namespace

void CodeDocumentDeleter::operator()(Scintilla::IDocument *document) const {
  delete static_cast<LexDocument *>(document);
}

CodeDocumentView codeDocumentView(CodeDocument &document) {
  return CodeDocumentView(new LexDocument(document));
}

void CodeDocument::reset(std::string_view utf8) {
  text_.assign(utf8);
  lineStates_.clear();
  styles_.assign(text_.size(), 0);
  indexLines();
}

void CodeDocument::append(std::string_view utf8) {
  if (utf8.empty()) {
    return;
  }
  const size_t previousLength = text_.size();
  text_.append(utf8);
  // New bytes have no style, which is what a lexer expects for text it has not
  // reached. Everything before keeps the style its last pass gave it.
  styles_.resize(text_.size(), 0);
  // Appends cannot move an existing line start. Scan only the new bytes and
  // retain the states the lexer wrote for all previous lines.
  if (lineStarts_.empty()) {
    lineStarts_.push_back(0);
  }
  for (size_t index = previousLength; index < text_.size(); index++) {
    if (text_[index] == '\n') {
      lineStarts_.push_back(index + 1);
    }
  }
  lineStates_.resize(lineStarts_.size(), 0);
}

void CodeDocument::indexLines() {
  lineStarts_.clear();
  lineStarts_.push_back(0);
  for (size_t index = 0; index < text_.size(); index++) {
    if (text_[index] == '\n') {
      lineStarts_.push_back(index + 1);
    }
  }
  // No entry past a last line that has no newline. Scintilla has no such line,
  // and a lexer that asks for that line's end, as Rust and Bash do to close a
  // line comment, would be told it stops one byte short. `lineStart` already
  // answers the text's length for any line past the last.
  lineStates_.resize(lineStarts_.size(), 0);
}

size_t CodeDocument::lineIndexAt(size_t position) const {
  const auto next = std::upper_bound(lineStarts_.begin(), lineStarts_.end(), position);
  return static_cast<size_t>(next - lineStarts_.begin()) - 1;
}

size_t CodeDocument::lineStart(size_t line) const {
  return line < lineStarts_.size() ? lineStarts_[line] : text_.size();
}

size_t CodeDocument::lineStartAt(size_t position) const {
  return lineStart(lineIndexAt(std::min(position, text_.size())));
}

size_t CodeDocument::unstableStart() const {
  if (text_.empty() || text_.back() == '\n') {
    return text_.size();
  }
  return lineStartAt(text_.size() - 1);
}

int CodeDocument::lineState(size_t line) const {
  return line < lineStates_.size() ? lineStates_[line] : 0;
}

int CodeDocument::setLineState(size_t line, int state) {
  if (line >= lineStates_.size()) {
    lineStates_.resize(line + 1, 0);
  }
  lineStates_[line] = state;
  return state;
}

char CodeDocument::styleAt(size_t position) const {
  return position < styles_.size() ? styles_[position] : 0;
}

void CodeDocument::clearStylesFrom(size_t position) {
  if (position >= styles_.size()) {
    return;
  }
  std::fill(styles_.begin() + static_cast<ptrdiff_t>(position), styles_.end(), 0);
}

void CodeDocument::writeStyles(size_t start, const char *bytes, size_t count) {
  if (count == 0 || start + count > styles_.size()) {
    return;
  }
  std::memcpy(styles_.data() + start, bytes, count);
}

void CodeDocument::fillStyles(size_t start, size_t count, char style) {
  if (count == 0 || start + count > styles_.size()) {
    return;
  }
  std::fill(styles_.begin() + static_cast<ptrdiff_t>(start), styles_.begin() + static_cast<ptrdiff_t>(start + count), style);
}

} // namespace downstream
