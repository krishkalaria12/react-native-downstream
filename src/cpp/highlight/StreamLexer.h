#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace downstream {

struct StreamLexerModule;
using CodeHighlighterResolver = const void *(*)();
bool codeHighlighterAvailable();
void installCodeHighlighterResolver(CodeHighlighterResolver resolver);

/// One instance of the vendored lexer module, driven as a line stream.
///
/// Instances are per thread: the parser colors blocks on a worker pool, and an
/// instance is not thread-safe. A block does not own one. Its resume state is a
/// small image of the lexer's saved state, so whichever thread colors the block
/// next restores the image into its own instance and carries on.
///
/// A stream is fed whole lines, so every chunk but the last ends in a newline;
/// the module carries multi-line constructs, embedded languages, and Markdown
/// fences from one chunk to the next. The last chunk may end mid-line.
class StreamLexer {
public:
  /// A record's token id for a line terminator, which carries no color.
  static constexpr uint32_t kLineEnd = 0xffffffffu;

  /// The calling thread's lexer, created on first use. Null only when an
  /// instance could not be made.
  static StreamLexer *current();
  /// Instances created since the process started. A document with no code
  /// block never creates one.
  static uint64_t created();
  /// Drops the calling thread's instance, for a lexer that trapped.
  static void discard();

  StreamLexer(const StreamLexer &) = delete;
  StreamLexer &operator=(const StreamLexer &) = delete;
  ~StreamLexer();

  /// Starts a stream for module language `language`: at the start of a
  /// document when `image` is null, otherwise where the stream that captured
  /// `image` stopped. A captured image may be empty: trailing zero bytes are
  /// trimmed. False when the module trapped.
  bool start(uint8_t language, const std::vector<uint8_t> *image);

  /// Lexes `bytes`, valid UTF-8, as the stream's next chunk. Fills `records`
  /// with (endUtf16, tokenId) pairs, each ending where the next begins and the
  /// first beginning at the chunk's start. False when the module trapped.
  bool lex(std::string_view bytes, std::vector<uint32_t> *records);

  /// The stream's state after the last chunk, for `start` to resume from.
  /// False when the module trapped.
  bool capture(std::vector<uint8_t> *image);

private:
  StreamLexer();

  std::unique_ptr<StreamLexerModule> module_;
};

} // namespace downstream
