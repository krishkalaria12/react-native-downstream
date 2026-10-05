#pragma once

#include "Document.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace downstream {

/// Contract limit for one canonical source, in UTF-8 bytes.
constexpr size_t kMaxSourceBytes = 1024 * 1024;

/**
 * Which syntax the parse recognizes: math delimiters, the inline tags it
 * renders, and md4c's own script syntaxes.
 *
 * Parse options rather than a resolved boolean, because whether `$x$` is math
 * has to be decided before anything is laid out. The bits match the native
 * `parseFlags` prop in `src/parseFlags.ts`, so the same number travels from JavaScript
 * to here without a translation step.
 */
namespace ParseFlags {

/// A document may contain math at all. Off means every formula stays literal
/// and the parse is byte-for-byte what it was before math existed.
constexpr uint32_t Enabled = 1u << 0;
/// `$…$` is inline math. Off by default, as in Streamdown, so currency is text.
constexpr uint32_t SingleDollar = 1u << 1;
/// `\(…\)` and `\[…\]` are math. On by default; Streamdown does not parse them.
constexpr uint32_t Brackets = 1u << 2;
/// `<br>`, `<sub>` and `<sup>` are recognized in inline HTML pieces. On by
/// default at the resolver level; off means every piece stays literal, exactly
/// today's parse.
constexpr uint32_t InlineHtml = 1u << 3;
/// md4c's `^x^` superscripts. Off by default.
constexpr uint32_t SuperscriptSyntax = 1u << 4;
/// md4c's `~x~` subscripts. Off by default.
constexpr uint32_t SubscriptSyntax = 1u << 5;

/// What `undefined` resolves to, and therefore what the codegen default is.
constexpr uint32_t Default = Enabled | Brackets;

constexpr uint32_t All = Enabled | SingleDollar | Brackets | InlineHtml | SuperscriptSyntax | SubscriptSyntax;

} // namespace ParseFlags

/// A parse's options, as the props and a stream's options resolve to.
struct ParseOptions {
  uint32_t flags{ParseFlags::Default};

  bool enabled() const { return (flags & ParseFlags::Enabled) != 0; }
  bool singleDollar() const { return (flags & ParseFlags::SingleDollar) != 0; }
  bool brackets() const { return (flags & ParseFlags::Brackets) != 0; }
  bool inlineHtml() const { return (flags & ParseFlags::InlineHtml) != 0; }
  bool superscriptSyntax() const { return (flags & ParseFlags::SuperscriptSyntax) != 0; }
  bool subscriptSyntax() const { return (flags & ParseFlags::SubscriptSyntax) != 0; }
  /// True when a document under these options parses exactly as it did before
  /// math existed. The flag is the only thing that has to be off, because every
  /// other bit is read through it.
  bool inert() const { return !enabled(); }

  bool operator==(const ParseOptions &) const = default;
};

enum class ParseStatus : uint8_t {
  Ok = 0,
  InvalidInput,
  InputLimit,
  ParseFailed,
};

/// Half-open range of UTF-8 bytes in the parsed source.
struct ByteRange {
  uint32_t start{0};
  uint32_t end{0};
};

struct ParseResult {
  ParseStatus status{ParseStatus::Ok};
  /// Set only when status is Ok.
  DocumentHandle document;
  std::string message;
  /// Containers and tables opened while parsing, including ones that produced
  /// no block. IDs are assigned in this order, so a parse spliced after
  /// another can offset its IDs by the earlier counts.
  uint32_t containerCount{0};
  uint32_t tableCount{0};
  /// Source text that produced each block's visible text, parallel to
  /// document->blocks. A block without source text has an empty range.
  std::vector<ByteRange> blockSource;
  /// The options this parse ran under. Parse options are part of a source's
  /// identity, so a caller caching parses keys on it as well.
  ParseOptions parse;
};

/// Parses a complete document with md4c's GitHub dialect, the same parser
/// options as the fixture oracle, and projects it into a render model.
/// Thread-safe; each call is independent.
///
/// `options` defaults to the same default the `math` prop resolves to, so a
/// caller that says nothing gets bracket math. A parse with math disabled
/// produces exactly the document it produced before math existed.
ParseResult parseMarkdown(std::string_view source, ParseOptions options = ParseOptions());

/// Parses a region of a larger document, as a stream's unstable tail is. The
/// result matches a full parse of the same bytes at the same offset, because
/// md4c's check that a list item cannot begin with two blank lines reads the
/// last line record as a block record and so depends on each line's byte offset
/// modulo 256. Returns ranges relative to the region, not the document.
ParseResult parseMarkdownAt(std::string_view region, size_t origin, ParseOptions options = ParseOptions());

/// Allocates an ID for a derived immutable snapshot without reparsing it.
uint64_t allocateDocumentId();

/// Stable machine-readable code for the public error contract.
const char *errorCode(ParseStatus status);

} // namespace downstream
