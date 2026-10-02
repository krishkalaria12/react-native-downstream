#pragma once

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "FenceMeta.h"
#include "math/MathLayout.h"

namespace downstream {

/// Half-open range of UTF-16 code units in the canonical source string.
struct SourceRange {
  uint32_t start{0};
  uint32_t end{0};

  bool operator==(const SourceRange &other) const {
    return start == other.start && end == other.end;
  }
};

enum class BlockKind : uint8_t {
  Paragraph = 0,
  Heading = 1,
  CodeBlock = 2,
  HtmlBlock = 3,
  ThematicBreak = 4,
  TableRow = 5,
  MathBlock = 6,
  Image = 7,
};

enum class ContainerKind : uint8_t {
  Quote = 0,
  ListItem = 1,
  Admonition = 2,
  Footnote = 3,
};

enum class TaskState : uint8_t {
  None = 0,
  Unchecked = 1,
  Checked = 2,
};

/// Vertical gap before a block. Platforms map these to point values.
enum class BlockSpacing : uint8_t {
  None = 0,
  Tight = 1,
  Normal = 2,
};

enum class CellAlign : uint8_t {
  Default = 0,
  Left = 1,
  Center = 2,
  Right = 3,
};

namespace InlineStyle {
constexpr uint32_t Strong = 1u << 0;
constexpr uint32_t Emphasis = 1u << 1;
constexpr uint32_t Strikethrough = 1u << 2;
constexpr uint32_t Code = 1u << 3;
constexpr uint32_t Link = 1u << 4;
constexpr uint32_t ImageAlt = 1u << 5;
constexpr uint32_t FootnoteRef = 1u << 6;
constexpr uint32_t Math = 1u << 7;
} // namespace InlineStyle

/// A maximal run of block text sharing one style set. Offsets are UTF-16
/// code units into Block::text. Runs never overlap; unstyled text has no run.
struct InlineRun {
  uint32_t start{0};
  uint32_t end{0};
  uint32_t styles{0};
  /// Index into Document::links, or -1.
  int32_t link{-1};
};

/// The class of a code token. Platform palettes map these to colors, so the
/// classes are shared between iOS and Android while the colors are not.
enum class TokenClass : uint8_t {
  Plain = 0,
  Comment,
  Keyword,
  Type,
  String,
  Number,
  Constant,
  Regex,
  Function,
  Property,
  Operator,
  Preprocessor,
  Tag,
  Attribute,
  Inserted,
  Deleted,
  Heading,
  Count,
};

/// A run of code text in one token class. Offsets are UTF-16 code units into
/// Block::text, as with InlineRun. Runs are sorted and never overlap, and
/// Plain text has no run, so a document with no tokens is a plain one.
struct TokenRun {
  uint32_t start{0};
  uint32_t end{0};
  TokenClass token{TokenClass::Plain};
};

/// One level of block nesting around a leaf block, outermost first.
struct Container {
  ContainerKind kind{ContainerKind::Quote};
  /// Unique within the document; adjacent blocks share it when they belong
  /// to the same quote or list item.
  uint32_t id{0};
  /// ListItem and Footnote: true only on the first leaf block of the item,
  /// where the marker is drawn.
  bool showsMarker{false};
  TaskState task{TaskState::None};
  /// Rendered marker for list items and footnotes ("•", "3.", "1."), or the
  /// admonition type. Markers are decoration and are not part of the text.
  std::u16string marker;
};

struct TableCell {
  uint32_t start{0};
  uint32_t end{0};
  CellAlign align{CellAlign::Default};
};

/**
 * Which delimiters a formula was written with.
 *
 * The builder needs this rather than the flag that produced it, because two
 * forms the parser folds onto the same span are drawn differently: `$$x$$` in a
 * sentence is text style, and `\(` was rewritten to `$$` only so md4c would
 * recognize it, so the original bytes are what say which it was.
 */
enum class MathDelimiter : uint8_t {
  /// `$…$`, which is only math when `singleDollarTextMath` is on.
  Dollar = 0,
  DoubleDollar,
  /// `\(…\)`.
  Paren,
  /// `\[…\]`.
  Bracket,
  /// A `$$` line that opened a fence.
  Fence,
  /// A ```` ```math ```` fence.
  MathFence,
};

/**
 * One formula.
 *
 * Inline math covers its literal text in `Block::text`, delimiters included, so
 * a fallback, a copy, and the display a reader had before math existed are all
 * the same string and every offset, link range, and table cell boundary is
 * unchanged. A platform decides at draw time whether the run is typeset or
 * literal, so one document serves both without the parse knowing the answer.
 */
struct MathSpan {
  /// UTF-16 into `Block::text`: the literal, delimiters included.
  uint32_t start{0};
  uint32_t end{0};
  /// The TeX between the delimiters, UTF-8, with newlines as written.
  std::string tex;
  /// Display style: a math block, `\[…\]`, and a promoted lone `$$…$$`.
  bool display{false};
  MathDelimiter delimiter{MathDelimiter::DoubleDollar};
  /// Null until a layout pass fills it in, and when the formula failed. An
  /// inline span carries its layout through the document rather than through
  /// props, so a formula that has been laid out once is not laid out again.
  MathLayout::Handle layout;
  /// The math engine's message when the formula failed. Empty otherwise, and
  /// empty for a formula that has not been laid out yet, so "not laid out" and
  /// "laid out and fine" are not confused with "failed".
  std::string error;

  /**
   * Identity for a committed block.
   *
   * `layout` and `error` are excluded on purpose: they are filled in after the
   * block is built, and a block whose formula is the same has the same identity
   * whether or not a pass has run against it yet. The committed-segment cache
   * keys on identity, so a layout that changed identity would rebuild every
   * later snapshot's rendering.
   */
  bool operator==(const MathSpan &other) const {
    return start == other.start && end == other.end && tex == other.tex && display == other.display &&
        delimiter == other.delimiter;
  }
};

/**
 * An image.
 *
 * Recorded for every image, loadable or not, so a run's alt text can name the
 * image it belongs to and a failed or blocked image has a record to fall back
 * from. `loadable` is a property of the URL alone: the prefix list is a view
 * prop, so the document is the same whatever an app is willing to fetch, and
 * the policy is applied where the box is decided.
 */
struct Image {
  /// The destination, entity-decoded, UTF-8.
  std::string url;
  /// Plain alt text, as written.
  std::u16string alt;
  /// The optional title, which is never drawn.
  std::string title;
  /// The `![…](…)` occurrence in the source, like a link's.
  SourceRange range;
  /// `isLoadableImageUrl(url)`: HTTPS, complete, with a host and no control
  /// characters or spaces.
  bool loadable{false};
  /// The enclosing link for `[![a](img)](href)`, or -1.
  int32_t link{-1};
};

/// A leaf block: the unit of text layout. Container blocks (quotes, lists)
/// are flattened into each leaf's container path.
struct Block {
  /// Nonzero only for a completed block whose source boundary is proven
  /// stable. The same identity survives later append-only snapshots.
  uint64_t identity{0};
  /// UTF-16 range of the canonical source text that produced this block's
  /// text, excluding block markers. Set only when identity is nonzero.
  SourceRange sourceRange;
  BlockKind kind{BlockKind::Paragraph};
  uint8_t headingLevel{0};
  bool tableHeader{false};
  /// True when md4c ended this fenced block at the end of the input with no
  /// closing fence after its last line. That is what makes it the block a
  /// writer is still typing into, which is where a caret belongs and where a
  /// further snapshot will append without reparsing. False for an indented code
  /// block, which can also continue, and for a fence that closed.
  bool fenceOpen{false};
  /// TableRow: unique per table, shared by all rows of one table.
  uint32_t table{0};
  BlockSpacing spacingBefore{BlockSpacing::Normal};
  std::vector<Container> containers;
  /// Visible text. Hard line breaks and code lines are '\n'; table cells are
  /// separated by '\t'. Thematic breaks have empty text.
  std::u16string text;
  std::vector<InlineRun> runs;
  std::vector<TableCell> cells;
  /// Fenced code info string language, UTF-8.
  std::string language;
  /// The whole fence info string, UTF-8, as written after the backticks.
  /// `language` is its first word. Kept whole so a renderer receives the raw
  /// meta string, as Streamdown's `CustomRendererProps.meta` does.
  std::string info;
  /// What `info` says about presentation, parsed once where md4c reported the
  /// block. The header label, the gutter, and the download name all read this
  /// one struct rather than each scanning `info` its own way. CodeBlock only.
  FenceMeta meta;
  /// CodeBlock only: syntax tokens. Empty when the language is unknown, the
  /// block is over the highlight limit, or highlighting is disabled. Tokens
  /// live here rather than in runs so the open-fence fast path and inline-code
  /// semantics stay untouched.
  std::vector<TokenRun> tokens;
  /// True when highlighting finished or deliberately skipped this exact text.
  /// A later full parse may reuse its tokens; a budget-cut pass must retry.
  bool highlightComplete{false};
  /// Paragraph, heading, table row: the inline formulas, sorted by `start`.
  /// A MathBlock holds exactly one, display style, covering the whole text.
  ///
  /// Sorted and non-overlapping, as runs are, so a draw pass walks the block's
  /// text once and never has to test every formula against every character.
  std::vector<MathSpan> math;
  /// Image blocks only: indexes into `Document::images`, in source order.
  /// Several images in one paragraph stack vertically rather than sharing a row,
  /// which is what an image-only paragraph becomes.
  std::vector<uint32_t> images;
};

/// An actionable link. Links that fail the URL policy are plain text and are
/// not listed here.
struct Link {
  std::string url;
  /// The link's Markdown occurrence in the source, including delimiters.
  SourceRange range;
};

/// A list whose leading items are immutable and shared between documents.
/// Streaming snapshots share their committed blocks this way, so each new
/// snapshot copies only its active tail. Reads behave like a vector.
template <typename T>
class SharedList {
public:
  /// Immutable leading items, stored in the chunks they were added in.
  class Prefix {
  public:
    size_t size() const { return items_.size(); }
    const T &operator[](size_t index) const { return *items_[index]; }

    /// Returns a new prefix with `chunk` appended. `prefix` may be null and
    /// is not changed, so documents that share it are unaffected.
    static std::shared_ptr<const Prefix> extend(const std::shared_ptr<const Prefix> &prefix, std::vector<T> chunk) {
      auto next = std::make_shared<Prefix>();
      if (prefix) {
        next->chunks_ = prefix->chunks_;
        next->items_ = prefix->items_;
      }
      auto stored = std::make_shared<const std::vector<T>>(std::move(chunk));
      next->items_.reserve(next->items_.size() + stored->size());
      for (const T &item : *stored) {
        next->items_.push_back(&item);
      }
      next->chunks_.push_back(std::move(stored));
      return next;
    }

  private:
    std::vector<std::shared_ptr<const std::vector<T>>> chunks_;
    std::vector<const T *> items_;
  };

  class const_iterator {
  public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = T;
    using difference_type = std::ptrdiff_t;
    using pointer = const T *;
    using reference = const T &;

    const_iterator(const SharedList *list, size_t index) : list_(list), index_(index) {}
    reference operator*() const { return (*list_)[index_]; }
    pointer operator->() const { return &(*list_)[index_]; }
    const_iterator &operator++() {
      index_++;
      return *this;
    }
    bool operator==(const const_iterator &other) const { return index_ == other.index_; }

  private:
    const SharedList *list_;
    size_t index_;
  };

  SharedList() = default;
  SharedList(std::shared_ptr<const Prefix> prefix, std::vector<T> tail)
      : prefix_(std::move(prefix)), tail_(std::move(tail)) {}

  size_t size() const { return sharedSize() + tail_.size(); }
  bool empty() const { return size() == 0; }
  const T &operator[](size_t index) const {
    const size_t shared = sharedSize();
    return index < shared ? (*prefix_)[index] : tail_[index - shared];
  }
  const T &front() const { return (*this)[0]; }
  const T &back() const { return (*this)[size() - 1]; }
  const_iterator begin() const { return {this, 0}; }
  const_iterator end() const { return {this, size()}; }

  /// Number of leading items held in the shared prefix.
  size_t sharedSize() const { return prefix_ ? prefix_->size() : 0; }
  const std::shared_ptr<const Prefix> &prefix() const { return prefix_; }
  /// Items after the shared prefix, owned by this list.
  std::vector<T> &tail() { return tail_; }
  const std::vector<T> &tail() const { return tail_; }
  void push_back(T item) { tail_.push_back(std::move(item)); }

private:
  std::shared_ptr<const Prefix> prefix_;
  std::vector<T> tail_;
};

/// Immutable parse result shared by measurement and mounting.
struct Document {
  uint64_t id{0};
  SharedList<Block> blocks;
  SharedList<Link> links;
  /// Every image the document's blocks name, indexed by `Block::images`. A
  /// separate list rather than a field on the block because an image belongs to
  /// the document: two blocks cannot name the same image, and a linked image's
  /// link is a document-level record like any other.
  SharedList<Image> images;
  /// Number of leading blocks that no later input can change. A control, and
  /// text selection, turn on once the block they belong to is inside this
  /// prefix, so a finished block is usable while the rest still streams.
  ///
  /// It is not the same as `Block::identity`, which in full mode only means
  /// "unchanged since the previous snapshot", and a controlled source has no
  /// identities at all even though every one of its blocks is final.
  uint32_t settledBlocks{0};

  /// True while a stream is still arriving, which a session sets on every
  /// snapshot it publishes before `finish()`.
  ///
  /// It is the only way an `isAnimating` prop reaches the native rendering: a
  /// controlled document leaves it false, so a controlled view has to say so
  /// itself, and a stream view works it out from the stream. Nothing about the
  /// parse changes because of it, which is why a copy of the document is enough
  /// and the block storage stays shared.
  bool streaming{false};

  /// True when blocks [0, to) are all settled.
  bool isSettled(size_t to) const { return to <= settledBlocks; }
};

using DocumentHandle = std::shared_ptr<const Document>;

/// A copy of `document` whose first `settled` blocks are marked settled.
///
/// Copying a Document shares its committed prefix and copies its block tail,
/// which is what publishing a stream snapshot already does. Returns `document`
/// itself when the count already matches, so a caller that has nothing to
/// change pays nothing.
inline DocumentHandle withSettledBlocks(const DocumentHandle &document, uint32_t settled) {
  if (!document || document->settledBlocks == settled) {
    return document;
  }
  auto copy = std::make_shared<Document>(*document);
  copy->settledBlocks = settled;
  return copy;
}

} // namespace downstream
