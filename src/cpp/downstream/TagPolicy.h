#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace downstream {

/**
 * The inline styles a run can carry, as a bitmask.
 *
 * They live here rather than in `Document.h` because a run's styles are decided
 * in `Scan.cpp`'s tag table as much as they are by the builder, and a second
 * copy of the bits would be a second place they could disagree.
 */
namespace InlineStyle {
constexpr uint32_t Strong = 1u << 0;
constexpr uint32_t Emphasis = 1u << 1;
constexpr uint32_t Strikethrough = 1u << 2;
constexpr uint32_t Code = 1u << 3;
constexpr uint32_t Link = 1u << 4;
constexpr uint32_t ImageAlt = 1u << 5;
constexpr uint32_t FootnoteRef = 1u << 6;
constexpr uint32_t Math = 1u << 7;
constexpr uint32_t Superscript = 1u << 8;
constexpr uint32_t Subscript = 1u << 9;
/// `<kbd>`: a key cap, which draws as inline code with a border.
constexpr uint32_t Keyboard = 1u << 10;
/// `<mark>`: highlighted text.
constexpr uint32_t Highlight = 1u << 11;
/// `<u>` and `<ins>`: underlined text.
constexpr uint32_t Underline = 1u << 12;
/// `||spoiler||`: hidden until tapped, which is a reader's choice rather than a
/// style — but it travels as a run because the text is laid out whether or not
/// it is shown, exactly like a collapsed `<details>` body.
constexpr uint32_t Spoiler = 1u << 13;
} // namespace InlineStyle

/// How the builder treats a tag it recognised.
enum class TagAction : uint8_t {
  /// Keep the source as the text the writer sent. The default, and what a name
  /// with no table row gets.
  Literal = 0,
  /// The tag is hidden and its content is styled. `TagStyle` says which.
  Style,
  /// The tag is hidden and its content is styled as a link to `href`.
  Link,
  /// The tag is hidden and its content is kept unstyled, as a `span` is.
  Unwrap,
  /// The tag and everything inside it are dropped.
  DropContent,
  /// A comment. Emits nothing at all.
  Hide,
};

/// The inline styles a tag can set, which are the ones a run already carries.
enum class TagStyle : uint8_t {
  None = 0,
  Strong,
  Emphasis,
  Strikethrough,
  Code,
  Subscript,
  Superscript,
  Keyboard,
  Highlight,
  Underline,
  /// A hard line break, which counts as a `<br>` in the stretch rule.
  Break,
  /// One of the `Sub*`/`Sup*` pair with nothing open. Swallowed, because a
  /// writer who means a superscript never writes `<sup/>`.
  VoidScript,
};

/** A tag the app allowed beyond the built-in table, with its allowed attributes. */
struct AllowedTag {
  /// Lowercased, as HTML element names are compared.
  std::string name;
  /// The attribute names this tag may carry, lowercased. Empty allows none.
  std::vector<std::string> attributes;

  bool operator==(const AllowedTag &) const = default;
};

/**
 * Which tags a document may use, beyond the built-in table.
 *
 * Part of the parse options rather than a view prop, because whether a tag runs
 * at all decides what runs exist and therefore what a copy and an export
 * contain — the same reason the other parse bits are where they are. It
 * compares by value so a caller caching parses keys on it as well.
 */
struct TagPolicy {
  /// The app's own tags, as `allowedTags` named them. Sorted by name, which is
  /// what makes the comparison and the lookup both a binary search.
  std::vector<AllowedTag> tags;
  /**
   * A real HTML element the table has no row for is unwrapped, so its text
   * survives and its markup does not.
   *
   * On by default, and the decided behaviour: Streamdown unwraps these too. What
   * it also does — and what this never does — is unwrap a name that is *not* an
   * element, which is how `Array<string>` becomes an unknown element whose
   * source reaches the screen. `false` is the escape hatch for an app that wants
   * every unrecognised tag's markup kept.
   */
  bool unwrapUnknownElements{true};
  /**
   * A tag's content is the raw source between its open and close tags, rather
   * than Markdown parsed from it.
   *
   * For every allowed tag, not per tag: an app asking for `<user>Bob</user>` to
   * be a name is asking for it not to be a paragraph, and having to say so per
   * tag would mean a policy object per tag for one boolean.
   */
  bool literalTagContent{false};

  bool operator==(const TagPolicy &) const = default;
};

/// The empty policy: the built-in table, every real element unwrapped, every
/// other name literal.
TagPolicy defaultTagPolicy();

/**
 * A tag policy from the flat list `src/allowedTags.ts` writes.
 *
 * `-1,name,attribute,-1,name,…` in name order. The sentinel is what tells the
 * reader where one tag's attributes stop, which a plain run of strings does not;
 * anything that is not that shape is refused whole, because a policy read half
 * right would allow an attribute nobody named.
 */
TagPolicy tagPolicyFromStrings(const std::vector<std::string> &flat);

} // namespace downstream