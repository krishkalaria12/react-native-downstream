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
 * Which math delimiters the parse recognizes.
 *
 * Parse options rather than a resolved boolean, because whether `$x$` is math
 * has to be decided before anything is laid out. The bits match the native
 * `mathFlags` prop in `src/math.ts`, so the same number travels from JavaScript
 * to here without a translation step.
 */
namespace MathParseFlags {

/// A document may contain math at all. Off means every formula stays literal
/// and the parse is byte-for-byte what it was before math existed.
constexpr uint32_t Enabled = 1u << 0;
/// `$…$` is inline math. Off by default, as in Streamdown, so currency is text.
constexpr uint32_t SingleDollar = 1u << 1;
/// `\(…\)` and `\[…\]` are math. On by default; Streamdown does not parse them.
constexpr uint32_t Brackets = 1u << 2;

/// What `undefined` resolves to, and therefore what the codegen default is.
constexpr uint32_t Default = Enabled | Brackets;

constexpr uint32_t All = Enabled | SingleDollar | Brackets;

} // namespace MathParseFlags

/// A parse's math options, as the props and a stream's options resolve to.
struct MathOptions {
  uint32_t flags{MathParseFlags::Default};

  bool enabled() const { return (flags & MathParseFlags::Enabled) != 0; }
  bool singleDollar() const { return (flags & MathParseFlags::SingleDollar) != 0; }
  bool brackets() const { return (flags & MathParseFlags::Brackets) != 0; }
  /// True when a document under these options parses exactly as it did before
  /// math existed. The flag is the only thing that has to be off, because every
  /// other bit is read through it.
  bool inert() const { return !enabled(); }

  bool operator==(const MathOptions &) const = default;
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
  /// The math options this parse ran under. Parse options are part of a
  /// source's identity, so a caller caching parses keys on it as well.
  MathOptions math;
};

/// Parses a complete document with md4c's GitHub dialect, the same parser
/// options as the fixture oracle, and projects it into a render model.
/// Thread-safe; each call is independent.
///
/// `options` defaults to the same default the `math` prop resolves to, so a
/// caller that says nothing gets bracket math. A parse with math disabled
/// produces exactly the document it produced before math existed.
ParseResult parseMarkdown(std::string_view source, MathOptions options = MathOptions());

/// Parses a region of a larger document, as a stream's unstable tail is. The
/// result matches a full parse of the same bytes at the same offset, because
/// md4c's check that a list item cannot begin with two blank lines reads the
/// last line record as a block record and so depends on each line's byte offset
/// modulo 256. Returns ranges relative to the region, not the document.
ParseResult parseMarkdownAt(std::string_view region, size_t origin, MathOptions options = MathOptions());

/// Allocates an ID for a derived immutable snapshot without reparsing it.
uint64_t allocateDocumentId();

/// Stable machine-readable code for the public error contract.
const char *errorCode(ParseStatus status);

} // namespace downstream
