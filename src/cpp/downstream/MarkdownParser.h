#pragma once

#include "Document.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace downstream {

/// Contract limit for one canonical source, in UTF-8 bytes.
constexpr size_t kMaxSourceBytes = 1024 * 1024;

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
};

/// Parses a complete document with md4c's GitHub dialect, the same parser
/// options as the fixture oracle, and projects it into a render model.
/// Thread-safe; each call is independent.
ParseResult parseMarkdown(std::string_view source);

/// Parses a region of a larger document, as a stream's unstable tail is. The
/// result matches a full parse of the same bytes at the same offset, because
/// md4c's check that a list item cannot begin with two blank lines reads the
/// last line record as a block record and so depends on each line's byte offset
/// modulo 256. Returns ranges relative to the region, not the document.
ParseResult parseMarkdownAt(std::string_view region, size_t origin);

/// Allocates an ID for a derived immutable snapshot without reparsing it.
uint64_t allocateDocumentId();

/// Stable machine-readable code for the public error contract.
const char *errorCode(ParseStatus status);

} // namespace downstream
