#pragma once

#include "Document.h"

#include <cstddef>
#include <string>
#include <string_view>

namespace downstream {

/// Contract limit for one canonical source, in UTF-8 bytes.
constexpr size_t kMaxSourceBytes = 1024 * 1024;

enum class ParseStatus : uint8_t {
  Ok = 0,
  InvalidInput,
  InputLimit,
  ParseFailed,
};

struct ParseResult {
  ParseStatus status{ParseStatus::Ok};
  /// Set only when status is Ok.
  DocumentHandle document;
  std::string message;
};

/// Parses a complete document with md4c's GitHub dialect, the same parser
/// options as the fixture oracle, and projects it into a render model.
/// Thread-safe; each call is independent.
ParseResult parseMarkdown(std::string_view source);

/// Allocates an ID for a derived immutable snapshot without reparsing it.
uint64_t allocateDocumentId();

/// Stable machine-readable code for the public error contract.
const char *errorCode(ParseStatus status);

} // namespace downstream
