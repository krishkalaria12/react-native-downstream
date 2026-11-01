#pragma once

#include "Document.h"
#include "TagPolicy.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
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
/// `/…`, `./…`, `../…` and `#…` are links rather than plain text. Off by
/// default, because a relative reference names no target until the app has
/// resolved it against an origin it was given, and Streamdown shows it as text
/// until it has one too.
constexpr uint32_t LinkRelative = 1u << 6;
/// A bare address is not a `mailto:` link. Off by default: a reader who wrote
/// one meant to write it, and Streamdown has no way to turn it off either.
constexpr uint32_t NoAutolinkMailto = 1u << 7;
/// A bare `http://` address, and the `http://` a `www.` autolink is rewritten
/// to, is not a link.
constexpr uint32_t NoAutolinkHttp = 1u << 8;
/// A bare `https://` address is not a link.
constexpr uint32_t NoAutolinkHttps = 1u << 9;
/// CJK-friendly emphasis and autolink boundaries, as the
/// [markdown-cjk-friendly](https://github.com/tats-u/markdown-cjk-friendly)
/// amendment defines them. On by default: a document without CJK in it parses
/// identically either way, which is what the differential test proves.
constexpr uint32_t Cjk = 1u << 10;
/// `==highlight==`, md4c's own mark syntax. Off by default: it is not GitHub
/// dialect, and a chat model that writes `==` around a version number does not
/// mean a highlighter.
constexpr uint32_t HighlightSyntax = 1u << 11;
/// `__underline__`, md4c's own underline syntax. Off by default, for the same
/// reason — and because it takes `_` away from emphasis, so turning it on
/// changes what `_x_` means everywhere.
constexpr uint32_t UnderlineSyntax = 1u << 12;
/// `||spoiler||`, md4c's own spoiler syntax. Off by default: it hides text
/// behind a tap, which is an app's decision about its own messages.
constexpr uint32_t SpoilerSyntax = 1u << 13;
/// An allowed tag's content is unstyled text rather than parsed Markdown. Off
/// by default: `<user>**Bob**</user>` styles Bob unless the app says its
/// custom tags are not Markdown.
constexpr uint32_t LiteralTagContent = 1u << 14;
/// A view without the code plugin never starts a lexer, even if it is installed.
constexpr uint32_t NoCodeHighlight = 1u << 15;

/// What `undefined` resolves to, and therefore what the codegen default is.
///
/// `Cjk` is in the default because it costs nothing for the text it does not
/// change, and the differential test is what keeps that promise checkable. The
/// link bits are not: they change what a reader can tap, which is an app's
/// decision.
constexpr uint32_t Default = Enabled | Brackets | Cjk;

constexpr uint32_t All = Enabled | SingleDollar | Brackets | InlineHtml | SuperscriptSyntax |
    SubscriptSyntax | LinkRelative | NoAutolinkMailto | NoAutolinkHttp | NoAutolinkHttps | Cjk |
    HighlightSyntax | UnderlineSyntax | SpoilerSyntax | LiteralTagContent | NoCodeHighlight;

} // namespace ParseFlags

/// A parse's options, as the props and a stream's options resolve to.
struct ParseOptions {
  uint32_t flags{ParseFlags::Default};
  /// Which HTML tags the source may use, and what happens to one with no row of
  /// its own. Compared by value, so it is part of a source's identity in the
  /// same way the flags are.
  TagPolicy tags;

  // Constructors rather than aggregate initialisation, because a caller that
  // names only a bitmask says so rather than leaving the tag policy to default,
  // and a caller that sets both cannot leave one out by accident.
  ParseOptions() = default;
  explicit ParseOptions(uint32_t flagsIn) : flags(flagsIn) {}
  ParseOptions(uint32_t flagsIn, TagPolicy tagsIn) : flags(flagsIn), tags(std::move(tagsIn)) {}

  /// Relative references are links rather than plain text.
  bool relativeLinks() const { return (flags & ParseFlags::LinkRelative) != 0; }

  bool enabled() const { return (flags & ParseFlags::Enabled) != 0; }
  bool singleDollar() const { return (flags & ParseFlags::SingleDollar) != 0; }
  bool brackets() const { return (flags & ParseFlags::Brackets) != 0; }
  bool inlineHtml() const { return (flags & ParseFlags::InlineHtml) != 0; }
  bool superscriptSyntax() const { return (flags & ParseFlags::SuperscriptSyntax) != 0; }
  bool subscriptSyntax() const { return (flags & ParseFlags::SubscriptSyntax) != 0; }
  bool cjk() const { return (flags & ParseFlags::Cjk) != 0; }
  bool highlightSyntax() const { return (flags & ParseFlags::HighlightSyntax) != 0; }
  bool underlineSyntax() const { return (flags & ParseFlags::UnderlineSyntax) != 0; }
  bool spoilerSyntax() const { return (flags & ParseFlags::SpoilerSyntax) != 0; }
  bool literalTagContent() const { return (flags & ParseFlags::LiteralTagContent) != 0; }
  bool codeHighlighting() const { return (flags & ParseFlags::NoCodeHighlight) == 0; }

  /**
   * Whether a bare address of this protocol may become a link.
   *
   * The one place the three autolink bits are read, so the builder, a link's
   * own record and a test cannot disagree about which protocol is allowed. An
   * explicit `[text](mailto:…)` never comes through here: it is not an autolink,
   * and a writer who typed the brackets meant the link.
   */
  bool autolinksMailto() const { return (flags & ParseFlags::NoAutolinkMailto) == 0; }
  bool autolinksHttp() const { return (flags & ParseFlags::NoAutolinkHttp) == 0; }
  bool autolinksHttps() const { return (flags & ParseFlags::NoAutolinkHttps) == 0; }

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

/// One list that is not inside any container, as md4c closed it or as it
/// stood when the input ran out.
struct ListSummary {
  bool ordered{false};
  /// The bullet (`-`, `+`, `*`) or, for an ordered list, the delimiter (`.`, `)`).
  char mark{0};
  bool tight{true};
  /// The number the next item would take. Ordered lists only.
  uint32_t nextNumber{0};
  uint32_t items{0};
  /// Container id of the list's last item.
  uint32_t lastItem{0};
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
  /// Original source ranges recognized by md4c as link definitions, in order.
  /// Footnotes are excluded because they produce document-global output.
  std::vector<ByteRange> referenceDefinitions;
  uint32_t referenceOutputUsed{0};
  bool referenceOutputLimited{false};
  /// Top-level lists in source order. A stream reads the last one to commit a
  /// list's finished items while the list is still open.
  std::vector<ListSummary> lists;
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

/// Parses a tail with previously confirmed definition-only source replayed.
/// References retain md4c normalization and first-definition precedence; output
/// ranges are relative to the tail, as with parseMarkdownAt.
///
/// `lead`, when present, is one list item that stands in for the open list the
/// tail continues. It is parsed before the tail and its block, container and
/// list item are removed from the result, so the tail's items are numbered and
/// spaced as the same list's. A lead that does not come back as exactly one
/// block in one list item, because the tail did not continue its list, fails
/// the parse with `ParseFailed`.
ParseResult parseMarkdownWithReferences(std::string_view region, size_t origin,
                                       std::string_view definitions, ParseOptions options = ParseOptions(),
                                       uint32_t referenceBudget = UINT32_MAX, std::string_view lead = {});

/// Allocates an ID for a derived immutable snapshot without reparsing it.
uint64_t allocateDocumentId();

/// Stable machine-readable code for the public error contract.
const char *errorCode(ParseStatus status);

} // namespace downstream
