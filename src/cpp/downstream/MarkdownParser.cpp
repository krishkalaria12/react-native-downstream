#include "MarkdownParser.h"

#include "LinkPolicy.h"
#include "MathScan.h"
#include "Scan.h"
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

/// "No offset in the block's text", for a span that has not seen text yet. A
/// block's text is bounded by `kMaxSourceBytes`, so this cannot collide.
constexpr uint32_t kNoBlockOffset = static_cast<uint32_t>(-1);

std::atomic<uint64_t> nextDocumentId{1};

/**
 * Whether a fence's info string says its contents are a formula.
 *
 * `math` only, matched case-insensitively, and only as the first word so
 * `mathematics` is not a formula. `latex` and `tex` deliberately stay code: a
 * writer who fenced those was showing source, and rendering it would throw away
 * what they wrote. GitHub renders ```` ```math ````, and so does Streamdown
 * through rehype-katex's `language-math` handling, so this is parity.
 */
bool isMathFenceLanguage(std::string_view language) {
  return language.size() == 4 && (language[0] == 'm' || language[0] == 'M') &&
      (language[1] == 'a' || language[1] == 'A') && (language[2] == 't' || language[2] == 'T') &&
      (language[3] == 'h' || language[3] == 'H');
}

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
  DocumentBuilder(std::string_view source, MathOptions math)
      : source_(source), parse_(source), brackets_(), math_(math), offsets_(source),
        document_(std::make_shared<Document>()) {
    document_->id = allocateDocumentId();
    if (math_.enabled()) {
      // The rewrite is the same length as the source, so every offset md4c
      // reports is also an offset into `source_`. That is the whole reason
      // this is a rewrite rather than a translation table: `parse_` is what
      // md4c sees, and `source_` is what a reader should see, and they are the
      // same bytes at the same offsets.
      brackets_ = scan::rewriteMathDelimiters(source, math_.brackets());
      if (!brackets_.empty()) {
        parse_ = brackets_.text;
      }
    }
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
      case MD_BLOCK_CODE: {
        const auto *code = static_cast<MD_BLOCK_CODE_DETAIL *>(detail);
        const std::string language = attributeToUtf8(code->lang);
        const bool mathFence = code->fence_char == '$' || isMathFenceLanguage(language);
        beginLeaf(mathFence ? BlockKind::MathBlock : BlockKind::CodeBlock);
        current_->language = language;
        current_->info = attributeToUtf8(code->info);
        if (mathFence) {
          // A `$$` fence has no info string by construction, and a `math` fence
          // is named `math` for the same reason a code fence is named after its
          // language: it is how a writer says the block is a formula. The
          // delimiter reaches the block's own MathSpan when the leaf is
          // finished, because that is where the block's text is complete.
          fenceMathDelimiter_ = code->fence_char == '$' ? MathDelimiter::Fence : MathDelimiter::MathFence;
        } else {
          fenceMathDelimiter_ = MathDelimiter::MathFence;
          // Parsed once, here, so the open-fence fast path copies a parsed meta
          // rather than re-parsing the string on every snapshot.
          current_->meta = parseFenceMeta(current_->info);
          if (code->fence_char != 0) {
            // Record the fence's own run so `fenceClosed` can be decided when
            // the block finishes. The backward scan works because the info
            // string sits right after the run on the opener line.
            const size_t infoAt = code->info.text != nullptr
                ? static_cast<size_t>(code->info.text - parse_.data())
                : 0;
            size_t runEnd = infoAt;
            while (runEnd > 0 && source_[runEnd - 1] == ' ') {
              runEnd--;
            }
            size_t runStart = runEnd;
            while (runStart > 0 && source_[runStart - 1] == code->fence_char) {
              runStart--;
            }
            fenceMarkerScratch_ = code->fence_char;
            fenceLengthScratch_ = static_cast<uint32_t>(runEnd - runStart);
            fenceRunStart_ = runStart;
            size_t newline = source_.find('\n', fenceRunStart_);
            // A fence with no info string has nothing to scan back from, so its
            // run is read off the opener line itself: at most three spaces of
            // indent and then the run, which is the rule that made it a fence.
            if (fenceLengthScratch_ == 0) {
              const size_t lineStart = runEnd == 0 ? 0 : source_.rfind('\n', runEnd - 1) + 1;
              size_t at = lineStart;
              size_t spaces = 0;
              while (at < source_.size() && source_[at] == ' ' && spaces < 3) {
                at++;
                spaces++;
              }
              size_t runStartOnLine = at;
              while (runStartOnLine < source_.size() && source_[runStartOnLine] == code->fence_char) {
                runStartOnLine++;
              }
              fenceLengthScratch_ = static_cast<uint32_t>(runStartOnLine - at);
              fenceRunStart_ = at;
              newline = source_.find('\n', fenceRunStart_);
            }
            fenceLineEnd_ = newline == std::string_view::npos ? source_.size() : newline + 1;
          }
        }
        break;
      }
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
        const auto *image = static_cast<MD_SPAN_IMG_DETAIL *>(detail);
        SpanFrame frame;
        frame.image = true;
        frame.cursorAtEnter = cursor_;
        // Recorded whether or not the URL may be fetched, so a run's alt text
        // can name its image and a failed or blocked one has a record to fall
        // back from. `loadable` is a property of the URL alone; the app's prefix
        // list is a view prop and is applied where the box is decided.
        Image record;
        record.url = attributeToUtf8(image->src);
        record.title = attributeToUtf8(image->title);
        record.loadable = isLoadableImageUrl(record.url);
        // A linked image's press reaches `onLinkPress`, which is the enclosing
        // link's own record, so the image names it rather than repeating it.
        record.link = currentLink();
        frame.imageIndex = static_cast<int32_t>(document_->images.size());
        document_->images.push_back(std::move(record));
        spans_.push_back(frame);
        break;
      }
      case MD_SPAN_LATEXMATH:
      case MD_SPAN_LATEXMATH_DISPLAY: {
        // md4c has already resolved the delimiters and reports the contents as a
        // verbatim run, so the span contributes no text of its own: the
        // delimiters are re-added from the original bytes, and the TeX is the
        // run the text callback receives.
        mathDisplay_ = type == MD_SPAN_LATEXMATH_DISPLAY;
        mathDepth_++;
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
      case MD_SPAN_LATEXMATH:
      case MD_SPAN_LATEXMATH_DISPLAY:
        mathDepth_--;
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
    // A code block arrives as a counted verbatim line, and md4c's own NUL
    // splitter hands back the chunk that follows a NUL starting *at* that NUL:
    // it reports `text(MD_TEXT_NULLCHAR, "", 1)` and then calls back with the
    // pointer it has not advanced past the NUL and a length that still counts
    // it. The notice is the first callback, so this byte is a duplicate of it
    // rather than a new one, and skipping it leaves the chunk holding exactly
    // the code that follows. A NUL in prose is only ever the notice, and a code
    // span goes through the same splitter, so both arrive the same way.
    if (size > 0 && type != MD_TEXT_NULLCHAR) {
      while (size > 0 && *text == '\0') {
        text++;
        size--;
      }
    }
    if (size == 0) {
      return 0;
    }
    noteSourceText(text, size);

    std::u16string piece;
    const bool verbatim = type == MD_TEXT_LATEXMATH;
    const uint32_t before = static_cast<uint32_t>(current_ ? current_->text.size() : 0);
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
    if (verbatim) {
      // md4c hands over only the TeX, pointing just past the opening delimiter
      // and stopping just short of the closing one. Those two positions are the
      // only record of where the delimiters were, and reading the original
      // bytes there is what distinguishes `\(x\)` from `$$x$$` after both were
      // rewritten to the same span.
      // The verbatim run begins just past the opening delimiter and ends just
      // short of the closing one, so the delimiter's own length is read off the
      // bytes before it rather than assumed: a run of one dollar is a formula
      // when the app asked for one, and a constant two would read the character
      // in front of it as part of the delimiter.
      const size_t contentAt = static_cast<size_t>(text - parse_.data());
      mathDelimLength_ = 0;
      while (mathDelimLength_ < 2 && contentAt > mathDelimLength_ &&
             parse_[contentAt - mathDelimLength_ - 1] == '$') {
        mathDelimLength_++;
      }
      if (mathDelimLength_ == 0 || contentAt < mathDelimLength_) {
        mathDelimLength_ = 2;
        mathOpenOffset_ = kNoBlockOffset;
      } else {
        mathOpenOffset_ = static_cast<uint32_t>(contentAt - mathDelimLength_);
      }
      mathCloseOffset_ = static_cast<uint32_t>(contentAt + size);
      // The delimiters and the TeX are appended together, so nothing appends
      // the TeX on its own first: the literal a reader sees is one piece, and
      // the span covers all of it.
      appendMath(piece, mathDisplay_);
    } else {
      appendText(piece, currentStyles(), currentLink());
    }
    noteBlockText(before, static_cast<uint32_t>(current_->text.size()));
    return 0;
  }

  DocumentHandle finish() {
    finishLeaf();
    return document_;
  }

  uint32_t containerCount() const { return containerCount_; }
  uint32_t tableCount() const { return tableCount_; }
  std::vector<ByteRange> takeBlockSource() { return std::move(blockSource_); }
  /// The bytes to hand to md4c: the source, or the same-length rewrite.
  const std::string_view &parsedText() const { return parse_; }

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
    /// This frame's image's index into Document::images, or -1.
    int32_t imageIndex{-1};
    size_t cursorAtEnter{kUnknownOffset};
    size_t firstText{kUnknownOffset};
    size_t lastTextEnd{kUnknownOffset};
    /// The same span measured in the block's own text, which is what an image's
    /// alt text is a slice of. A source offset is not one: the block's text
    /// drops container markers and a code fence, so a source offset and a text
    /// offset are different numbers.
    uint32_t firstTextBlock{kNoBlockOffset};
    uint32_t lastTextBlock{kNoBlockOffset};
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

  /// Strips `depth` container lines from the front of `line`, as the reader's
  /// own content would be read. A quote level takes a `>` preceded by any
  /// amount of rubric indentation, plus one optional space after it.
  static std::optional<std::string_view> stripContainerLine(std::string_view line, size_t quoteDepth) {
    for (size_t level = 0; level < quoteDepth; level++) {
      size_t spaces = 0;
      while (spaces < line.size() && line[spaces] == ' ') {
        spaces++;
      }
      if (spaces >= line.size() || line[spaces] != '>') {
        return std::nullopt;
      }
      size_t past = spaces + 1;
      if (past < line.size() && line[past] == ' ') {
        past++;
      }
      line = line.substr(past);
    }
    return line;
  }

  /// Decides `fenceClosed` for a code block whose closing line must be found
  /// in the source: the line after the content must match `closesFence`
  /// relative to the opener's own column, after the container path is
  /// stripped. Fence lines longer than the opener close it, shorter or
  /// different-character runs do not, and no line closes a block whose fence
  /// ran to EOF.
  void finishLeaf() {
    if (!current_) {
      return;
    }
    if ((current_->kind == BlockKind::CodeBlock || current_->kind == BlockKind::HtmlBlock ||
         current_->kind == BlockKind::MathBlock) &&
        !current_->text.empty() && current_->text.back() == u'\n') {
      current_->text.pop_back();
      for (auto &run : current_->runs) {
        run.end = std::min<uint32_t>(run.end, static_cast<uint32_t>(current_->text.size()));
      }
    }
    if (current_->kind == BlockKind::CodeBlock && fenceMarkerScratch_ != 0) {
      size_t quoteDepth = 0;
      for (const auto &container : current_->containers) {
        if (container.kind == ContainerKind::Quote) {
          quoteDepth++;
        }
      }
      const size_t contentEnd = current_->text.empty() ? fenceLineEnd_ : leafEnd_;
      const size_t newline = fenceRunStart_ == 0 ? 0 : source_.rfind('\n', fenceRunStart_ - 1);
      const size_t openerLineStart = newline == std::string_view::npos ? 0 : newline + 1;
      // Compute the opener's column within the stripped opener directly from
      // the quote-stripped form of the opener line.
      std::string_view openerLine = source_.substr(openerLineStart, fenceLineEnd_ - openerLineStart);
      // Trim at the newline
      if (!openerLine.empty() && openerLine.back() == '\n') {
        openerLine.remove_suffix(1);
      }
      auto stripped = stripContainerLine(openerLine, quoteDepth);
      size_t rawCol = 0;
      if (stripped) {
        const size_t found = stripped->find(fenceMarkerScratch_);
        rawCol = found == std::string_view::npos ? 0 : found;
      }
      // The candidate is the line after the content: a block's own last line ends
      // with the newline `leafEnd_` points at, so the line that could close the
      // fence starts after it. A fence whose closer is the next line is what
      // `closesFence` then decides, with CommonMark's own rule for a shorter or
      // different-character run.
      const size_t afterContent = source_.find('\n', contentEnd);
      const size_t candidateStart = afterContent == std::string_view::npos ? source_.size() : afterContent + 1;
      auto candidate_op = [&]() -> bool {
        if (candidateStart >= source_.size()) {
          return false;
        }
        const size_t newline2 = source_.find('\n', candidateStart);
        std::string_view candidate =
            newline2 == std::string_view::npos ? source_.substr(candidateStart)
                                               : source_.substr(candidateStart, newline2 - candidateStart);
        auto s = stripContainerLine(candidate, quoteDepth);
        if (!s) {
          return false;
        }
        std::string_view rest = *s;
        if (rest.size() < rawCol) {
          return false;
        }
        if (rest.size() > rawCol && rest.substr(0, rawCol).find_first_not_of(' ') != std::string_view::npos) {
          return false;
        }
        rest = rest.substr(rawCol);
        return scan::closesFence(rest, fenceMarkerScratch_, fenceLengthScratch_);
      };
      current_->fenceMarker = fenceMarkerScratch_;
      current_->fenceLength = fenceLengthScratch_;
      current_->fenceClosed = candidate_op();
    }
    promoteLeaf();
    document_->blocks.push_back(std::move(*current_));
    current_.reset();
    if (leafStart_ == kUnknownOffset) {
      leafStart_ = leafEnd_ = lastLeafEnd_;
    }
    blockSource_.push_back({static_cast<uint32_t>(leafStart_), static_cast<uint32_t>(leafEnd_)});
    lastLeafEnd_ = leafEnd_;
    leafStart_ = kUnknownOffset;
    fenceMarkerScratch_ = 0;
    fenceLengthScratch_ = 0;
    fenceRunStart_ = 0;
    fenceLineEnd_ = 0;
  }

  /**
   * Turns two kinds of paragraph into blocks of their own.
   *
   * A paragraph whose only content is one display formula, and a paragraph
   * whose only content is images, are both things a reader sees as a block. The
   * decisions are here rather than in a platform because a segment is the unit
   * of layout, and a formula or a picture cannot be measured inside a text
   * segment. Both are the same rule Streamdown applies to its own markup: a
   * paragraph holding one such element drops its paragraph.
   *
   * Anything else keeps its kind, so a badge or a formula in the middle of a
   * sentence is still prose, which is the case that is common in chat and the
   * one that must not change.
   */
  void promoteLeaf() {
    if (current_->kind == BlockKind::MathBlock) {
      // The fence's contents are the whole text, so the block's one formula
      // covers it. A span is made here rather than in the text callback because
      // the delimiters are not part of the text: a fence's are the fence.
      MathSpan span;
      span.start = 0;
      span.end = static_cast<uint32_t>(current_->text.size());
      span.tex = utf16ToUtf8(current_->text);
      span.display = true;
      span.delimiter = fenceMathDelimiter_;
      current_->math = {std::move(span)};
      current_->language.clear();
      current_->info.clear();
      return;
    }
    if (current_->kind != BlockKind::Paragraph) {
      return;
    }
    if (promoteLoneMath()) {
      return;
    }
    promoteImageParagraph();
  }

  /**
   * True when this paragraph is one display formula and nothing else, in which
   * case it becomes a math block.
   *
   * `$$x$$` and `\[x\]` on one line, alone in their paragraph. Streamdown renders
   * this inline, and a model that writes it means display: a formula on its own
   * line, centred, is what the writer was asking for. `$$` on a line of its own
   * never reaches here — that is a fence, and md4c reports it as a block.
   */
  bool promoteLoneMath() {
    if (current_->math.size() != 1 || !current_->cells.empty()) {
      // Two formulas in one paragraph are two formulas, not one block, and a cell
      // means the paragraph is a table row, so in both cases this is not a
      // paragraph that is only its formula.
      return false;
    }
    const MathSpan &span = current_->math.front();
    if (!span.display || span.start != 0 || span.end != current_->text.size()) {
      return false;
    }
    // A run *inside* the formula is the formula's own: its delimiters and its TeX
    // are appended as one styled run so a platform can find a typeset run without
    // consulting the span. Rejecting every run therefore rejected every formula,
    // which is why a lone `$$x$$` stayed a paragraph. What matters is whether
    // anything *outside* the formula is styled, because that is the prose that
    // would have to come along.
    for (const InlineRun &run : current_->runs) {
      if (run.start < span.start || run.end > span.end) {
        return false;
      }
    }
    Block math = *current_;
    math.kind = BlockKind::MathBlock;
    math.runs.clear();
    math.cells.clear();
    // The block's own span is the paragraph's span, so the delimiter the writer
    // used survives the promotion: a `\[x\]` paragraph is a `\[` block.
    current_ = std::move(math);
    return true;
  }

  /**
   * True when this paragraph is images and only whitespace between them, in
   * which case it becomes an image block.
   *
   * Anything else in the paragraph keeps it a paragraph with alt text, as
   * before: `see ![the chart](x) above` is a sentence with a picture in it, and
   * turning that into a block would delete the sentence. Several images in one
   * paragraph stack vertically rather than sharing a row, which is the one
   * layout decision the promotion implies.
   */
  void promoteImageParagraph() {
    if (current_->images.empty() || !current_->cells.empty() || !current_->math.empty()) {
      return;
    }
    // Every unit of the block's text has to be an image's alt text or the
    // whitespace between two of them. Walking the text once is what keeps a
    // word from being silently dropped by the promotion, which is the failure
    // mode that would lose a reader's text.
    uint32_t cursor = 0;
    for (const uint32_t index : current_->images) {
      if (index >= document_->images.size()) {
        return;
      }
      const std::u16string &alt = document_->images[index].alt;
      if (cursor + alt.size() > current_->text.size() ||
          current_->text.compare(cursor, alt.size(), alt) != 0) {
        return;
      }
      cursor += static_cast<uint32_t>(alt.size());
      // Only a space or a soft break may separate two images, so a paragraph
      // with a comma between them stays a paragraph.
      while (cursor < current_->text.size() && (current_->text[cursor] == u'\n' || current_->text[cursor] == u' ')) {
        cursor++;
      }
    }
    if (cursor != current_->text.size()) {
      return;
    }
    // A linked image is promoted the same way: the picture is the link's hit
    // area, so it is the block, and the link's range is the image's own.
    current_->kind = BlockKind::Image;
    // The alt texts joined by newlines, so accessibility, export, and a
    // fallback all read one string and none of them needs the images.
    std::u16string alt;
    for (size_t i = 0; i < current_->images.size(); i++) {
      if (i > 0) {
        alt.push_back(u'\n');
      }
      alt.append(document_->images[current_->images[i]].alt);
    }
    current_->text = std::move(alt);
    current_->runs.clear();
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

  /// Records where in the block's own text each open span's text landed, so an
  /// image's alt text is a slice of the text a reader sees rather than a
  /// re-parse of the source.
  void noteBlockText(uint32_t before, uint32_t after) {
    for (auto &span : spans_) {
      if (span.firstTextBlock == kNoBlockOffset) {
        span.firstTextBlock = before;
      }
      span.lastTextBlock = after;
    }
  }

  /**
   * Records one inline formula, delimiters included.
   *
   * md4c reports only the TeX, so the delimiters are appended here from the
   * original bytes. That is what makes `Block::text` the literal the writer
   * sent: every offset, link range and table cell boundary downstream is
   * unchanged, a fallback shows exactly what was written, and a copy of the
   * text pastes as working Markdown into another editor.
   *
   * The delimiter is read from the original rather than inferred from the span
   * type, because `\(…\)` was rewritten to `$$…$$` before the parse and only
   * the original still says which one it was.
   */
  void appendMath(const std::u16string &piece, bool display) {
    if (!current_) {
      return;
    }
    auto &block = *current_;
    const uint32_t openAt = mathOpenOffset_;
    const uint32_t closeAt = mathCloseOffset_;
    // The styles of the run the formula sits in, plus the math bit. A formula
    // inside `**…**` is a bold formula, which is what the surrounding emphasis
    // means, and the bit is what lets a platform tell a typeset run from an
    // unstyled one without consulting the span.
    const uint32_t styles = currentStyles() | InlineStyle::Math;
    const int32_t link = currentLink();
    MathSpan span;
    span.start = static_cast<uint32_t>(block.text.size());
    span.tex = utf16ToUtf8(piece);
    span.display = bracketDisplayAt(openAt, display);
    span.delimiter = mathDelimiterAt(openAt, closeAt, display);
    // The literal, delimiters included, is what a reader sees and what a copy
    // yields, and it is the same string a document with math off produces for
    // the same source. Three `appendText` calls, which merge into one run
    // because they are contiguous and identically styled.
    appendDelimiter(openAt);
    appendText(piece, styles, link);
    appendDelimiter(closeAt);
    span.end = static_cast<uint32_t>(block.text.size());
    block.math.push_back(std::move(span));
  }

  /**
   * The delimiters, read from the original source.
   *
   * `mathDelimLength_` bytes, which is one for `$…$` and two for `$$…$$`. The
   * length has to be the opener's rather than a constant, because a single `$`
   * is a formula when the app asked for it and its closer is a single `$` too:
   * reading two bytes at a one-dollar delimiter would swallow the character
   * before it, which is how `That costs $x$` became `That costs  $x$$`.
   */
  void appendDelimiter(size_t at) {
    if (at == kNoBlockOffset || at + mathDelimLength_ > source_.size()) {
      current_->text.append(mathDelimLength_ == 1 ? u"$" : u"$$");
      return;
    }
    std::u16string delimiter;
    for (uint32_t i = 0; i < mathDelimLength_; i++) {
      delimiter.push_back(static_cast<char16_t>(source_[at + i]));
    }
    // Goes through `appendText` so a formula's delimiters are covered by the
    // same run as its TeX rather than sitting outside it.
    appendText(delimiter, currentStyles() | InlineStyle::Math, currentLink());
  }

  /**
   * Whether this formula is display style, correcting md4c's answer for a rewrite.
   *
   * A rewritten `\(` and a written `$$` are the same two bytes by the time md4c
   * sees them, so md4c calls both display. They are not the same thing: `\[…\]` is
   * display math and `\[…\)` is the inline form, and LaTeX's own convention is the
   * only thing that says which the writer meant. The pre-scan recorded it before the
   * rewrite, so this is where it is read back rather than guessed at.
   *
   * Anything the scan did not rewrite keeps md4c's answer, which is right for `$`
   * and `$$` and for a `$$` line fence.
   */
  bool bracketDisplayAt(size_t openAt, bool fromParser) const {
    for (const scan::MathPair &pair : brackets_.pairs) {
      if (pair.open == openAt) {
        return pair.display;
      }
    }
    return fromParser;
  }

  /// Which delimiters the writer used, from the original bytes.
  MathDelimiter mathDelimiterAt(size_t openAt, size_t closeAt, bool display) const {
    (void)closeAt;
    (void)display;
    if (openAt == kNoBlockOffset || openAt + 1 >= source_.size()) {
      return MathDelimiter::DoubleDollar;
    }
    const char first = source_[openAt];
    const char second = source_[openAt + 1];
    if (first == '\\' && second == '(') {
      return MathDelimiter::Paren;
    }
    if (first == '\\' && second == '[') {
      return MathDelimiter::Bracket;
    }
    if (first == '$' && second == '$') {
      return MathDelimiter::DoubleDollar;
    }
    // A single dollar only reaches here when the app asked for
    // `singleDollarTextMath`, because md4c never reports a one-dollar run
    // otherwise.
    return MathDelimiter::Dollar;
  }

  void noteSourceText(const MD_CHAR *text, MD_SIZE size) {
    if (text < parse_.data() || text >= parse_.data() + parse_.size()) {
      return;
    }
    const size_t begin = static_cast<size_t>(text - parse_.data());
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
    return scan::isUnfinishedDestination(source_, at);
  }

  bool isEscaped(size_t index) const {
    return scan::isEscaped(source_, index);
  }

  size_t findUnescaped(char needle, size_t from) const {
    return scan::findUnescaped(source_, needle, from);
  }

  /// `open` points at '('. Returns the offset after the matching ')'.
  size_t skipInlineDestination(size_t open) const {
    return scan::skipInlineDestination(source_, open);
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
      return;
    }

    if (span.image && span.imageIndex >= 0) {
      auto &image = document_->images.tail()[static_cast<size_t>(span.imageIndex)];
      if (range) {
        image.range.start = offsets_.toUtf16(range->first);
        image.range.end = offsets_.toUtf16(range->second);
      }
      // The alt text is the span's own slice of the block's text, so it is
      // exactly what a reader sees and what a copy produces, and a nested link
      // inside the alt contributes its text rather than its target.
      if (span.firstTextBlock != kNoBlockOffset && span.lastTextBlock > span.firstTextBlock) {
        image.alt = current_->text.substr(span.firstTextBlock, span.lastTextBlock - span.firstTextBlock);
      }
      if (current_) {
        current_->images.push_back(static_cast<uint32_t>(span.imageIndex));
      }
    }
  }

  /// The canonical source, as the writer sent it. Every byte a reader should
  /// see is read from here rather than from `parse_`, so a `\[` stays `\[`.
  std::string_view source_;
  /// What md4c parsed. Equal to `source_` unless the bracket pre-scan rewrote
  /// something, and the same length either way, so an offset into one is an
  /// offset into the other.
  std::string_view parse_;
  /// The rewritten source and the pairs it came from. Held for the whole parse
  /// because `parse_` is a view into it.
  scan::MathSource brackets_;
  MathOptions math_;
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
  /// Offset just after the newline ending a fenced block opener's line, so a
  /// never-empty code block can be anchored even when md4c emitted no text.
  size_t fenceLineEnd_{0};
  char fenceMarkerScratch_{0};
  uint32_t fenceLengthScratch_{0};
  size_t fenceRunStart_{0};
  size_t cursor_{0};
  unsigned tableRow_{0};
  uint32_t containerCount_{0};
  uint32_t tableCount_{0};
  int strongDepth_{0};
  int emphasisDepth_{0};
  int strikeDepth_{0};
  int codeDepth_{0};
  /// md4c does not nest math spans, so one flag is the whole display state.
  int mathDepth_{0};
  bool mathDisplay_{false};
  /// Where the verbatim run currently being handled had its delimiters.
  uint32_t mathOpenOffset_{kNoBlockOffset};
  uint32_t mathCloseOffset_{kNoBlockOffset};
  /// Dollars in the current span's delimiters: one or two, from the opener.
  uint32_t mathDelimLength_{2};
  /// Which fence a MathBlock came from, decided where the fence was reported and
  /// read when the block's own span is made.
  MathDelimiter fenceMathDelimiter_{MathDelimiter::Fence};
};

} // namespace

uint64_t allocateDocumentId() {
  return nextDocumentId.fetch_add(1, std::memory_order_relaxed);
}

ParseResult parseMarkdown(std::string_view source, MathOptions options) {
  ParseResult result;
  result.math = options;
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

  DocumentBuilder builder(source, options);
  const std::string_view &parsed = builder.parsedText();
  if (parsed.empty()) {
    result.document = builder.finish();
    result.blockSource = builder.takeBlockSource();
    return result;
  }
  MD_PARSER parser{};
  parser.abi_version = 0;
  // Math is the only extension the GitHub dialect does not include, and its
  // flags are off unless a caller asked for math, so with math disabled these
  // are exactly 0x6 and the parse is what it was before math existed.
  parser.flags = MD_DIALECT_GITHUB;
  if (options.enabled()) {
    parser.flags |= MD_FLAG_LATEXMATHSPANS;
    parser.flags |= MD_FLAG_LATEXMATHBLOCKS;
    if (!options.singleDollar()) {
      parser.flags |= MD_FLAG_LATEXMATHNOSINGLE;
    }
  }
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

  if (md_parse(parsed.data(), static_cast<MD_SIZE>(parsed.size()), &parser, &builder) != 0) {
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

ParseResult parseMarkdownAt(std::string_view region, size_t origin, MathOptions options) {
  const uint32_t pad = static_cast<uint32_t>(origin % 256);
  if (pad == 0) {
    return parseMarkdown(region, options);
  }
  std::string padded(pad, '\n');
  padded.append(region);
  ParseResult result = parseMarkdown(padded, options);
  if (result.status != ParseStatus::Ok) {
    return result;
  }
  for (ByteRange &range : result.blockSource) {
    range.start = range.start > pad ? range.start - pad : 0;
    range.end = range.end > pad ? range.end - pad : 0;
  }
  // The pad shifts source offsets, and a link's and an image's range are both
  // source offsets, so both lists move together. A range that was empty stays
  // empty: it names no source, and subtracting from zero would invent one.
  if (!result.document->links.empty() || !result.document->images.empty()) {
    auto shifted = std::make_shared<Document>(*result.document);
    for (Link &link : shifted->links.tail()) {
      if (link.range.end > link.range.start) {
        link.range.start -= pad;
        link.range.end -= pad;
      }
    }
    for (Image &image : shifted->images.tail()) {
      if (image.range.end > image.range.start) {
        image.range.start -= pad;
        image.range.end -= pad;
      }
    }
    result.document = shifted;
  }
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
