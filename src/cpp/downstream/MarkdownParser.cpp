#include "MarkdownParser.h"

#include "LinkPolicy.h"
#include "Unicode.h"

#include <md4c.h>

// Apps can link other md4c copies (Enriched Markdown vendors one). Unprefixed
// symbols would let the linker bind md_parse to a different md4c version.
#ifndef md_parse
#error "Build md4c with md_parse=downstream_md_parse; see Downstream.podspec and CMakeLists.txt."
#endif

extern "C" {
#include <entity.h>
}

#include <algorithm>
#include <cctype>
#include <atomic>
#include <optional>

namespace downstream {

namespace {

constexpr size_t kUnknownOffset = static_cast<size_t>(-1);

std::atomic<uint64_t> nextDocumentId{1};

std::u16string asciiToUtf16(std::string_view text) {
  return std::u16string(text.begin(), text.end());
}

void appendEntity(std::u16string &out, std::string_view entity) {
  if (entity.size() > 3 && entity[1] == '#') {
    uint32_t codePoint = 0;
    const bool hex = entity[2] == 'x' || entity[2] == 'X';
    for (size_t i = hex ? 3 : 2; i + 1 < entity.size(); i++) {
      const char c = entity[i];
      const uint32_t digit = c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10;
      codePoint = codePoint * (hex ? 16 : 10) + digit;
    }
    appendCodePoint(out, codePoint);
    return;
  }
  if (const ENTITY *match = entity_lookup(entity.data(), entity.size())) {
    appendCodePoint(out, match->codepoints[0]);
    if (match->codepoints[1] != 0) {
      appendCodePoint(out, match->codepoints[1]);
    }
    return;
  }
  appendUtf8(out, entity);
}

std::string attributeToUtf8(const MD_ATTRIBUTE &attribute) {
  if (attribute.text == nullptr || attribute.size == 0) {
    return {};
  }
  std::u16string decoded;
  for (size_t i = 0; attribute.substr_offsets[i] < attribute.size; i++) {
    const MD_OFFSET begin = attribute.substr_offsets[i];
    const MD_OFFSET end = attribute.substr_offsets[i + 1];
    const std::string_view piece(attribute.text + begin, end - begin);
    switch (attribute.substr_types[i]) {
      case MD_TEXT_NULLCHAR:
        decoded.push_back(kReplacementCharacter);
        break;
      case MD_TEXT_ENTITY:
        appendEntity(decoded, piece);
        break;
      default:
        appendUtf8(decoded, piece);
        break;
    }
  }
  return utf16ToUtf8(decoded);
}

class DocumentBuilder {
public:
  explicit DocumentBuilder(std::string_view source)
      : source_(source), offsets_(source), document_(std::make_shared<Document>()) {
    document_->id = allocateDocumentId();
  }

  int enterBlock(MD_BLOCKTYPE type, void *detail) {
    switch (type) {
      case MD_BLOCK_THEAD:
      case MD_BLOCK_TBODY:
      case MD_BLOCK_DOC:
        return 0;
      case MD_BLOCK_TH:
      case MD_BLOCK_TD:
        beginCell(type == MD_BLOCK_TH, static_cast<MD_BLOCK_TD_DETAIL *>(detail)->align);
        return 0;
      default:
        break;
    }

    finishLeaf();

    switch (type) {
      case MD_BLOCK_QUOTE:
        pushContainer(ContainerKind::Quote, u"", TaskState::None);
        break;
      case MD_BLOCK_UL: {
        const auto *ul = static_cast<MD_BLOCK_UL_DETAIL *>(detail);
        size_t unorderedDepth = 0;
        for (const auto &list : lists_) {
          unorderedDepth += list.ordered ? 0 : 1;
        }
        static const char16_t bullets[] = {u'\u2022', u'\u25E6', u'\u2023'};
        lists_.push_back({false, ul->is_tight != 0, 0, 0, bullets[std::min<size_t>(unorderedDepth, 2)], 0});
        break;
      }
      case MD_BLOCK_OL: {
        const auto *ol = static_cast<MD_BLOCK_OL_DETAIL *>(detail);
        lists_.push_back({true, ol->is_tight != 0, ol->start, ol->mark_delimiter, 0, 0});
        break;
      }
      case MD_BLOCK_LI: {
        const auto *li = static_cast<MD_BLOCK_LI_DETAIL *>(detail);
        std::u16string marker;
        if (!lists_.empty()) {
          auto &list = lists_.back();
          if (list.ordered) {
            marker = asciiToUtf16(std::to_string(list.nextNumber++));
            marker.push_back(static_cast<char16_t>(list.delimiter));
          } else {
            marker.push_back(list.bullet);
          }
        }
        TaskState task = TaskState::None;
        if (li->is_task) {
          task = li->task_mark == ' ' ? TaskState::Unchecked : TaskState::Checked;
        }
        pushContainer(ContainerKind::ListItem, std::move(marker), task);
        break;
      }
      case MD_BLOCK_ADMONITION: {
        const auto *admonition = static_cast<MD_BLOCK_ADMONITION_DETAIL *>(detail);
        std::string type = attributeToUtf8(admonition->type);
        pushContainer(ContainerKind::Admonition, utf8ToUtf16(type), TaskState::None);
        if (!type.empty()) {
          type[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(type[0])));
        }
        beginLeaf(BlockKind::Paragraph);
        appendText(utf8ToUtf16(type), InlineStyle::Strong, -1);
        finishLeaf();
        break;
      }
      case MD_BLOCK_FOOTNOTE_DEF_SECTION:
        // Definitions arrive after the body, out of source order.
        cursor_ = kUnknownOffset;
        beginLeaf(BlockKind::ThematicBreak);
        finishLeaf();
        break;
      case MD_BLOCK_FOOTNOTE_DEF: {
        const auto *footnote = static_cast<MD_BLOCK_FOOTNOTE_DEF_DETAIL *>(detail);
        std::u16string marker = asciiToUtf16(std::to_string(footnote->id));
        marker.push_back(u'.');
        pushContainer(ContainerKind::Footnote, std::move(marker), TaskState::None);
        cursor_ = kUnknownOffset;
        break;
      }
      case MD_BLOCK_HR:
        beginLeaf(BlockKind::ThematicBreak);
        break;
      case MD_BLOCK_H:
        beginLeaf(BlockKind::Heading);
        current_->headingLevel = static_cast<uint8_t>(static_cast<MD_BLOCK_H_DETAIL *>(detail)->level);
        break;
      case MD_BLOCK_CODE:
        beginLeaf(BlockKind::CodeBlock);
        current_->language = attributeToUtf8(static_cast<MD_BLOCK_CODE_DETAIL *>(detail)->lang);
        break;
      case MD_BLOCK_HTML:
        beginLeaf(BlockKind::HtmlBlock);
        break;
      case MD_BLOCK_P:
        beginLeaf(BlockKind::Paragraph);
        break;
      case MD_BLOCK_TABLE:
        tableRow_ = 0;
        tableCount_++;
        break;
      case MD_BLOCK_TR:
        beginLeaf(BlockKind::TableRow);
        current_->table = tableCount_;
        if (tableRow_++ > 0) {
          current_->spacingBefore = BlockSpacing::Tight;
        }
        break;
      default:
        break;
    }
    return 0;
  }

  int leaveBlock(MD_BLOCKTYPE type, void * /*detail*/) {
    switch (type) {
      case MD_BLOCK_TH:
      case MD_BLOCK_TD:
        if (current_ && !current_->cells.empty()) {
          current_->cells.back().end = static_cast<uint32_t>(current_->text.size());
        }
        return 0;
      case MD_BLOCK_THEAD:
      case MD_BLOCK_TBODY:
      case MD_BLOCK_TABLE:
        return 0;
      default:
        break;
    }

    finishLeaf();

    switch (type) {
      case MD_BLOCK_QUOTE:
      case MD_BLOCK_LI:
      case MD_BLOCK_ADMONITION:
      case MD_BLOCK_FOOTNOTE_DEF:
        if (!containers_.empty()) {
          containers_.pop_back();
        }
        break;
      case MD_BLOCK_UL:
      case MD_BLOCK_OL:
        if (!lists_.empty()) {
          lists_.pop_back();
        }
        break;
      default:
        break;
    }
    return 0;
  }

  int enterSpan(MD_SPANTYPE type, void *detail) {
    switch (type) {
      case MD_SPAN_EM:
        emphasisDepth_++;
        break;
      case MD_SPAN_STRONG:
        strongDepth_++;
        break;
      case MD_SPAN_DEL:
        strikeDepth_++;
        break;
      case MD_SPAN_CODE:
        codeDepth_++;
        break;
      case MD_SPAN_A: {
        const auto *a = static_cast<MD_SPAN_A_DETAIL *>(detail);
        SpanFrame frame;
        frame.image = false;
        frame.autolink = a->is_autolink != 0;
        frame.cursorAtEnter = cursor_;
        std::string url = attributeToUtf8(a->href);
        if (frame.autolink) {
          // A bare URL is only a target once its offset is known, because an
          // unfinished destination has to be left as text. A real link owns its
          // destination, so it can be recorded straight away.
          frame.autolinkUrl = std::move(url);
        } else if (isActionableUrl(url)) {
          frame.link = static_cast<int32_t>(document_->links.size());
          document_->links.push_back({std::move(url), {}});
        }
        spans_.push_back(frame);
        break;
      }
      case MD_SPAN_IMG: {
        SpanFrame frame;
        frame.image = true;
        frame.cursorAtEnter = cursor_;
        spans_.push_back(frame);
        break;
      }
      case MD_SPAN_FOOTNOTE_REF: {
        const auto *reference = static_cast<MD_SPAN_FOOTNOTE_REF_DETAIL *>(detail);
        ensureLeaf();
        appendText(asciiToUtf16(std::to_string(reference->id)), currentStyles() | InlineStyle::FootnoteRef,
                   currentLink());
        break;
      }
      default:
        // Other extensions are disabled by the parser flags.
        break;
    }
    return 0;
  }

  int leaveSpan(MD_SPANTYPE type, void * /*detail*/) {
    switch (type) {
      case MD_SPAN_EM:
        emphasisDepth_--;
        break;
      case MD_SPAN_STRONG:
        strongDepth_--;
        break;
      case MD_SPAN_DEL:
        strikeDepth_--;
        break;
      case MD_SPAN_CODE:
        codeDepth_--;
        break;
      case MD_SPAN_A:
      case MD_SPAN_IMG:
        closeLinkOrImage();
        break;
      default:
        break;
    }
    return 0;
  }

  int text(MD_TEXTTYPE type, const MD_CHAR *text, MD_SIZE size) {
    ensureLeaf();
    noteSourceText(text, size);

    std::u16string piece;
    switch (type) {
      case MD_TEXT_NULLCHAR:
        piece.push_back(kReplacementCharacter);
        break;
      case MD_TEXT_BR:
        piece.push_back(u'\n');
        break;
      case MD_TEXT_SOFTBR:
        // The fixture contract keeps source line breaks visible.
        piece.push_back(u'\n');
        break;
      case MD_TEXT_ENTITY:
        appendEntity(piece, std::string_view(text, size));
        break;
      default:
        appendUtf8(piece, std::string_view(text, size));
        break;
    }
    appendText(piece, currentStyles(), currentLink());
    return 0;
  }

  DocumentHandle finish() {
    finishLeaf();
    return document_;
  }

  uint32_t containerCount() const { return containerCount_; }
  uint32_t tableCount() const { return tableCount_; }
  std::vector<ByteRange> takeBlockSource() { return std::move(blockSource_); }

private:
  struct ListFrame {
    bool ordered;
    bool tight;
    unsigned nextNumber;
    char delimiter;
    char16_t bullet;
    size_t leaves;
  };

  struct ContainerFrame {
    Container container;
    bool markerPending;
  };

  /// Tracks a link or image so its source range can be recovered: md4c
  /// reports no span offsets, but text inside the span points into the
  /// source, which bounds the delimiters.
  struct SpanFrame {
    bool image{false};
    bool autolink{false};
    /// A bare URL, held until the first text offset reveals whether it is prose
    /// or the destination of a link or image that never closed.
    std::string autolinkUrl;
    int32_t link{-1};
    size_t cursorAtEnter{kUnknownOffset};
    size_t firstText{kUnknownOffset};
    size_t lastTextEnd{kUnknownOffset};
    /// Range of a nested image that precedes any text in this span.
    size_t childStart{kUnknownOffset};
    size_t childEnd{kUnknownOffset};
  };

  void pushContainer(ContainerKind kind, std::u16string marker, TaskState task) {
    Container container;
    container.kind = kind;
    container.id = ++containerCount_;
    container.task = task;
    container.marker = std::move(marker);
    containers_.push_back({std::move(container), kind == ContainerKind::ListItem || kind == ContainerKind::Footnote});
  }

  void beginLeaf(BlockKind kind) {
    finishLeaf();
    current_.emplace();
    current_->kind = kind;
    current_->containers.reserve(containers_.size());
    for (auto &frame : containers_) {
      Container container = frame.container;
      container.showsMarker = frame.markerPending;
      frame.markerPending = false;
      current_->containers.push_back(std::move(container));
    }
    current_->spacingBefore = spacingForNewLeaf();
    for (auto &list : lists_) {
      list.leaves++;
    }
  }

  BlockSpacing spacingForNewLeaf() const {
    if (document_->blocks.empty()) {
      return BlockSpacing::None;
    }
    if (lists_.empty()) {
      return BlockSpacing::Normal;
    }
    const auto &innermost = lists_.back();
    if (!innermost.tight) {
      return BlockSpacing::Normal;
    }
    if (innermost.leaves > 0) {
      return BlockSpacing::Tight;
    }
    // First leaf of a nested list directly inside a tight list item.
    if (lists_.size() > 1) {
      const auto &parent = lists_[lists_.size() - 2];
      if (parent.tight && parent.leaves > 0) {
        return BlockSpacing::Tight;
      }
    }
    return BlockSpacing::Normal;
  }

  /// Tight list items and footnote definitions carry inline content without
  /// a paragraph block.
  void ensureLeaf() {
    if (!current_) {
      beginLeaf(BlockKind::Paragraph);
    }
  }

  void finishLeaf() {
    if (!current_) {
      return;
    }
    if ((current_->kind == BlockKind::CodeBlock || current_->kind == BlockKind::HtmlBlock) &&
        !current_->text.empty() && current_->text.back() == u'\n') {
      current_->text.pop_back();
      for (auto &run : current_->runs) {
        run.end = std::min<uint32_t>(run.end, static_cast<uint32_t>(current_->text.size()));
      }
    }
    document_->blocks.push_back(std::move(*current_));
    current_.reset();
    if (leafStart_ == kUnknownOffset) {
      leafStart_ = leafEnd_ = lastLeafEnd_;
    }
    blockSource_.push_back({static_cast<uint32_t>(leafStart_), static_cast<uint32_t>(leafEnd_)});
    lastLeafEnd_ = leafEnd_;
    leafStart_ = kUnknownOffset;
  }

  void beginCell(bool header, MD_ALIGN align) {
    ensureLeaf();
    if (!current_->cells.empty()) {
      current_->text.push_back(u'\t');
    }
    current_->tableHeader = current_->tableHeader || header;
    TableCell cell;
    cell.start = static_cast<uint32_t>(current_->text.size());
    cell.end = cell.start;
    cell.align = static_cast<CellAlign>(align);
    current_->cells.push_back(cell);
  }

  uint32_t currentStyles() const {
    uint32_t styles = 0;
    if (strongDepth_ > 0 || (current_ && current_->kind == BlockKind::TableRow && current_->tableHeader)) {
      styles |= InlineStyle::Strong;
    }
    if (emphasisDepth_ > 0) {
      styles |= InlineStyle::Emphasis;
    }
    if (strikeDepth_ > 0) {
      styles |= InlineStyle::Strikethrough;
    }
    if (codeDepth_ > 0) {
      styles |= InlineStyle::Code;
    }
    for (const auto &span : spans_) {
      if (span.image) {
        styles |= InlineStyle::ImageAlt;
      }
    }
    if (currentLink() >= 0) {
      styles |= InlineStyle::Link;
    }
    return styles;
  }

  int32_t currentLink() const {
    for (auto it = spans_.rbegin(); it != spans_.rend(); ++it) {
      if (!it->image) {
        return it->link;
      }
    }
    return -1;
  }

  void appendText(const std::u16string &piece, uint32_t styles, int32_t link) {
    if (piece.empty()) {
      return;
    }
    auto &block = *current_;
    const auto start = static_cast<uint32_t>(block.text.size());
    block.text.append(piece);
    const auto end = static_cast<uint32_t>(block.text.size());
    if (styles == 0 && link < 0) {
      return;
    }
    if (!block.runs.empty()) {
      auto &last = block.runs.back();
      if (last.end == start && last.styles == styles && last.link == link) {
        last.end = end;
        return;
      }
    }
    block.runs.push_back({start, end, styles, link});
  }

  void noteSourceText(const MD_CHAR *text, MD_SIZE size) {
    if (text < source_.data() || text >= source_.data() + source_.size()) {
      return;
    }
    const size_t begin = static_cast<size_t>(text - source_.data());
    const size_t end = begin + size;
    if (leafStart_ == kUnknownOffset) {
      leafStart_ = begin;
      leafEnd_ = end;
    } else {
      leafStart_ = std::min(leafStart_, begin);
      leafEnd_ = std::max(leafEnd_, end);
    }
    for (auto &span : spans_) {
      if (span.firstText == kUnknownOffset) {
        span.firstText = begin;
        if (span.autolink) {
          // md4c autolinks a bare URL wherever it appears in text, so the `](`
          // of a link or image that never closed reaches here as literal text
          // and the URL would be linked anyway. That target is provisional: the
          // next chunk can extend the URL, or close the construct and remove it.
          // A completed link or image consumes its own destination, so only an
          // unfinished one leaves the `](` behind, and that one stays text.
          if (!isUnfinishedDestination(begin) && isActionableUrl(span.autolinkUrl)) {
            span.link = static_cast<int32_t>(document_->links.size());
            document_->links.push_back({std::move(span.autolinkUrl), {}});
          }
        }
      }
      span.lastTextEnd = span.lastTextEnd == kUnknownOffset ? end : std::max(span.lastTextEnd, end);
    }
    cursor_ = end;
  }

  /// True when a bare URL starting at `at` is the destination of a link or
  /// image whose opening `](` is still in the text. The bracket has to be a
  /// real one, since `\](` is an escaped literal bracket followed by a paren
  /// and leaves a URL in prose.
  bool isUnfinishedDestination(size_t at) const {
    return at >= 2 && source_[at - 1] == '(' && source_[at - 2] == ']' && !isEscaped(at - 2);
  }

  bool isEscaped(size_t index) const {
    size_t backslashes = 0;
    while (index > backslashes && source_[index - backslashes - 1] == '\\') {
      backslashes++;
    }
    return backslashes % 2 == 1;
  }

  size_t findUnescaped(char needle, size_t from) const {
    for (size_t i = from; i < source_.size(); i++) {
      if (source_[i] == needle && !isEscaped(i)) {
        return i;
      }
    }
    return kUnknownOffset;
  }

  /// `open` points at '('. Returns the offset after the matching ')'.
  size_t skipInlineDestination(size_t open) const {
    size_t depth = 0;
    char quote = 0;
    for (size_t i = open; i < source_.size(); i++) {
      const char c = source_[i];
      if (c == '\\') {
        i++;
        continue;
      }
      if (quote != 0) {
        if (c == quote) {
          quote = 0;
        }
        continue;
      }
      if (c == '<' && depth == 1) {
        const size_t close = findUnescaped('>', i + 1);
        if (close == kUnknownOffset) {
          return kUnknownOffset;
        }
        i = close;
      } else if (c == '"' || c == '\'') {
        quote = c;
      } else if (c == '(') {
        depth++;
      } else if (c == ')') {
        if (--depth == 0) {
          return i + 1;
        }
      }
    }
    return kUnknownOffset;
  }

  std::optional<std::pair<size_t, size_t>> spanRange(const SpanFrame &span) const {
    if (span.autolink) {
      if (span.firstText == kUnknownOffset) {
        return std::nullopt;
      }
      size_t start = span.firstText;
      size_t end = span.lastTextEnd;
      if (start > 0 && source_[start - 1] == '<') {
        start--;
        if (end < source_.size() && source_[end] == '>') {
          end++;
        }
      }
      return std::make_pair(start, end);
    }

    size_t anchor = span.firstText;
    if (span.childStart != kUnknownOffset && (anchor == kUnknownOffset || span.childStart < anchor)) {
      anchor = span.childStart;
    }

    size_t open = kUnknownOffset;
    if (anchor != kUnknownOffset) {
      const size_t floor = span.cursorAtEnter == kUnknownOffset ? 0 : span.cursorAtEnter;
      for (size_t i = anchor; i > floor; i--) {
        if (source_[i - 1] == '[' && !isEscaped(i - 1)) {
          open = i - 1;
          break;
        }
      }
    } else if (span.cursorAtEnter != kUnknownOffset) {
      open = findUnescaped('[', span.cursorAtEnter);
    }
    if (open == kUnknownOffset) {
      return std::nullopt;
    }
    size_t start = open;
    if (span.image && start > 0 && source_[start - 1] == '!') {
      start--;
    }

    size_t searchFrom = open + 1;
    for (const size_t candidate : {span.lastTextEnd, span.childEnd}) {
      if (candidate != kUnknownOffset) {
        searchFrom = std::max(searchFrom, candidate);
      }
    }
    const size_t close = findUnescaped(']', searchFrom);
    if (close == kUnknownOffset) {
      return std::nullopt;
    }
    size_t end = close + 1;
    if (end < source_.size() && source_[end] == '(') {
      const size_t destinationEnd = skipInlineDestination(end);
      if (destinationEnd != kUnknownOffset) {
        end = destinationEnd;
      }
    } else if (end < source_.size() && source_[end] == '[') {
      const size_t labelEnd = findUnescaped(']', end + 1);
      if (labelEnd != kUnknownOffset) {
        end = labelEnd + 1;
      }
    }
    return std::make_pair(start, end);
  }

  void closeLinkOrImage() {
    if (spans_.empty()) {
      return;
    }
    const SpanFrame span = spans_.back();
    spans_.pop_back();

    const auto range = spanRange(span);
    if (range) {
      cursor_ = std::max(cursor_ == kUnknownOffset ? 0 : cursor_, range->second);
      if (!spans_.empty()) {
        auto &parent = spans_.back();
        if (parent.childStart == kUnknownOffset) {
          parent.childStart = range->first;
        }
        parent.childEnd = range->second;
      }
    }

    if (!span.image && span.link >= 0) {
      auto &link = document_->links.tail()[static_cast<size_t>(span.link)];
      if (range) {
        link.range.start = offsets_.toUtf16(range->first);
        link.range.end = offsets_.toUtf16(range->second);
      } else if (span.firstText != kUnknownOffset) {
        link.range.start = offsets_.toUtf16(span.firstText);
        link.range.end = offsets_.toUtf16(span.lastTextEnd);
      }
    }
  }

  std::string_view source_;
  Utf16OffsetMap offsets_;
  std::shared_ptr<Document> document_;
  std::optional<Block> current_;
  std::vector<ContainerFrame> containers_;
  std::vector<ListFrame> lists_;
  std::vector<SpanFrame> spans_;
  std::vector<ByteRange> blockSource_;
  size_t leafStart_{kUnknownOffset};
  size_t leafEnd_{0};
  size_t lastLeafEnd_{0};
  size_t cursor_{0};
  unsigned tableRow_{0};
  uint32_t containerCount_{0};
  uint32_t tableCount_{0};
  int strongDepth_{0};
  int emphasisDepth_{0};
  int strikeDepth_{0};
  int codeDepth_{0};
};

} // namespace

uint64_t allocateDocumentId() {
  return nextDocumentId.fetch_add(1, std::memory_order_relaxed);
}

ParseResult parseMarkdown(std::string_view source) {
  ParseResult result;
  if (source.size() > kMaxSourceBytes) {
    result.status = ParseStatus::InputLimit;
    result.message = "Markdown source exceeds 1 MiB of UTF-8.";
    return result;
  }
  if (!isValidUtf8(source)) {
    result.status = ParseStatus::InvalidInput;
    result.message = "Markdown source is not well-formed Unicode.";
    return result;
  }

  DocumentBuilder builder(source);
  if (source.empty()) {
    result.document = builder.finish();
    result.blockSource = builder.takeBlockSource();
    return result;
  }
  MD_PARSER parser{};
  parser.abi_version = 0;
  parser.flags = MD_DIALECT_GITHUB;
  parser.enter_block = [](MD_BLOCKTYPE type, void *detail, void *userdata) {
    return static_cast<DocumentBuilder *>(userdata)->enterBlock(type, detail);
  };
  parser.leave_block = [](MD_BLOCKTYPE type, void *detail, void *userdata) {
    return static_cast<DocumentBuilder *>(userdata)->leaveBlock(type, detail);
  };
  parser.enter_span = [](MD_SPANTYPE type, void *detail, void *userdata) {
    return static_cast<DocumentBuilder *>(userdata)->enterSpan(type, detail);
  };
  parser.leave_span = [](MD_SPANTYPE type, void *detail, void *userdata) {
    return static_cast<DocumentBuilder *>(userdata)->leaveSpan(type, detail);
  };
  parser.text = [](MD_TEXTTYPE type, const MD_CHAR *text, MD_SIZE size, void *userdata) {
    return static_cast<DocumentBuilder *>(userdata)->text(type, text, size);
  };

  if (md_parse(source.data(), static_cast<MD_SIZE>(source.size()), &parser, &builder) != 0) {
    result.status = ParseStatus::ParseFailed;
    result.message = "The Markdown parser could not complete.";
    return result;
  }
  result.document = builder.finish();
  result.containerCount = builder.containerCount();
  result.tableCount = builder.tableCount();
  result.blockSource = builder.takeBlockSource();
  return result;
}

const char *errorCode(ParseStatus status) {
  switch (status) {
    case ParseStatus::Ok:
      return "";
    case ParseStatus::InvalidInput:
      return "invalid_input";
    case ParseStatus::InputLimit:
      return "input_limit";
    case ParseStatus::ParseFailed:
      return "parse_failed";
  }
  return "parse_failed";
}

} // namespace downstream
