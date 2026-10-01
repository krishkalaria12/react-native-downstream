#pragma once

#include "Document.h"

#include <cstddef>
#include <string>
#include <string_view>

namespace downstream {

/// What a control button serializes a block into. `Text` is the code a copy
/// puts on the clipboard; the rest are the table formats.
enum class ExportFormat : uint8_t {
  /// `Block::text` verbatim: the code as the writer sent it, never a source
  /// slice and never a layout placeholder.
  Text = 0,
  Csv = 1,
  Tsv = 2,
  /// A GFM table, with inline styles and column alignment written back.
  Markdown = 3,
  /// A table element for the clipboard, so a paste lands as a table.
  Html = 4,
};

/// The separator a `,`, `;`, or tab CSV uses. `auto` is resolved by the
/// platform from the current locale before the call, so the engine never needs
/// a locale database.
constexpr char16_t kDefaultCsvSeparator = u',';

/// Serializes blocks [from, to) of a document.
///
/// Reads the document and nothing else: no platform calls, no globals, and no
/// state between calls, so iOS and Android write the same bytes for the same
/// block. The result is UTF-16, which converts to `NSString` and `jstring`
/// without a re-encode.
///
/// A table range is every row of one table. A code range is one block. An empty
/// range is an empty string.
std::u16string exportBlocks(const Document &document, size_t from, size_t to, ExportFormat format,
                            char16_t csvSeparator = kDefaultCsvSeparator);

/// The code of one block, exactly as displayed.
std::u16string exportCodeText(const Document &document, size_t index);

/// The file name a code download saves as, extension included.
///
/// The base name is `meta.title`, which the parser already resolved from
/// `title="app.ts"`, `filename="app.ts"`, or the path after `lang:`; failing
/// that the caller's configured base, and failing that `fallback`. Only the last
/// path component is kept, reserved characters and control characters are
/// removed, the result is capped, and it never starts with a dot. A fence name
/// that already carries an extension keeps it; otherwise the extension comes
/// from the language, and an unknown language or an indented block gets `txt`.
std::string downloadName(const FenceMeta &meta, std::string_view language, const std::string &configuredBase,
                         const std::string &fallback);

/// The file name a table download saves as: `<base>.<extension>`, with the
/// same sanitizing as a code name.
std::string tableDownloadName(const std::string &configuredBase, std::string_view extension);

} // namespace downstream
