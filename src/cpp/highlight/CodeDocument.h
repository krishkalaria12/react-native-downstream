#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace Scintilla {
class IDocument;
}

namespace downstream {

/// Scintilla's `IDocument` over one code block's UTF-8 text, so a Lexilla lexer
/// can colour it line by line and resume from any line start.
///
/// Modelled on Lexilla's own `test/TestDocument.cxx`. Line states, style bytes,
/// and line starts survive appends, which is what lets a streaming block be
/// re-lexed from its last complete line while lexers that look back read the
/// state they would have found in an editor. Folding levels and decorations
/// are unused by every vendored lexer and do nothing here.
class CodeDocument {
public:
  /// Replaces the text, discarding every cached style and line state.
  void reset(std::string_view utf8);
  /// Takes the conversion buffer directly when the caller owns it, avoiding
  /// another full-text copy on a rebuilt fence.
  void resetOwned(std::string utf8);
  /// Appends to the text, keeping every style and line state below the old
  /// end. New bytes carry no style, which is what a lexer expects for text it
  /// has not reached.
  void append(std::string_view utf8);

  const std::string &text() const { return text_; }
  size_t length() const { return text_.size(); }
  /// Number of lines, counting the empty line after a final '\n'.
  size_t lineCount() const { return lineStarts_.size(); }
  size_t lineIndexAt(size_t position) const;
  size_t lineStart(size_t line) const;
  /// Start of the line holding `position`.
  size_t lineStartAt(size_t position) const;
  /// Where the text stops being final: length() when it ends with a newline,
  /// and otherwise the start of the last line. Nothing before it can change, so
  /// a lexer may resume there and a resume must start there, or a token the
  /// next chunk completes would be decided without it.
  size_t unstableStart() const;
  int lineState(size_t line) const;
  int setLineState(size_t line, int state);

  char styleAt(size_t position) const;
  /// Style bytes, one per byte of text, as Lexilla wrote them.
  const std::vector<char> &styles() const { return styles_; }
  void clearStylesFrom(size_t position);
  void writeStyles(size_t start, const char *bytes, size_t count);
  void fillStyles(size_t start, size_t count, char style);

private:
  void indexLines();

  std::string text_;
  std::vector<size_t> lineStarts_;
  std::vector<int> lineStates_;
  std::vector<char> styles_;
};

/// Lexilla's `IDocument` has no virtual destructor, so the view's deleter
/// knows the concrete type. Defined in CodeDocument.cpp.
struct CodeDocumentDeleter {
  void operator()(Scintilla::IDocument *document) const;
};

/// A Lexilla view of `document`, valid until the document is moved. The
/// vendored lexers are compiled with `-DScintilla=downstream_scintilla`, so
/// this declaration and the definition in CodeDocument.cpp see the same
/// renamed namespace.
using CodeDocumentView = std::unique_ptr<Scintilla::IDocument, CodeDocumentDeleter>;
CodeDocumentView codeDocumentView(CodeDocument &document);

} // namespace downstream
