#include "MarkdownParser.h"

#include "LinkPolicy.h"
#include "MathScan.h"
#include "Direction.h"
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
#include <atomic>
#include <cctype>
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
  DocumentBuilder(std::string_view source, ParseOptions options)
      : source_(source), parse_(source), brackets_(), options_(options), offsets_(source),
        document_(std::make_shared<Document>()) {
    document_->id = allocateDocumentId();
    // The flag is the transport and the policy field is the read site, so one
    // assignment here is the whole mapping: the builder owns its options, and
    // a later `setParseTags` replaces the policy whole rather than merging
    // into it.
    options_.tags.literalTagContent = options_.literalTagContent();
    if (options_.enabled()) {
      // The rewrite is the same length as the source, so every offset md4c
      // reports is also an offset into `source_`. That is the whole reason
      // this is a rewrite rather than a translation table: `parse_` is what
      // md4c sees, and `source_` is what a reader should see, and they are the
      // same bytes at the same offsets.
      brackets_ = scan::rewriteMathDelimiters(source, options_.brackets());
      if (!brackets_.empty()) {
        parse_ = brackets_.text;
      }
    }
  }

  int enterBlock(MD_BLOCKTYPE type, void *detail) {
    // A literal tag's scope is its own block: an unclosed `<user>` does not
    // reach into the next paragraph, because the policy promises the raw
    // source between an open and a close in one block.
    literalDepth_ = 0;
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

    // A `<details>` block is a container rather than a leaf, so its tags are read
    // before the leaf is finished: the opener pushes a frame the body blocks land
    // inside, and the closer pops it. See `consumeContainerHtml`.
    if (type == MD_BLOCK_HTML && options_.inlineHtml()) {
      consumeContainerHtml();
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
    // A span boundary ends any pending break stretch: the styles were
    // captured when the stretch opened, so flushing now keeps the appended
    // newlines in callback order and the inner span's text offsets from
    // absorbing the breaks between spans.
    flushStretch(false);
    // Inside a literal tag no span opens: no emphasis, no link records, no
    // images. The text still arrives through the text callback, unstyled but
    // present.
    if (inLiteralTag()) {
      return 0;
    }
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
      case MD_SPAN_MARK:
        highlightDepth_++;
        break;
      case MD_SPAN_U:
        underlineDepth_++;
        break;
      case MD_SPAN_SPOILER:
        spoilerDepth_++;
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
        } else if (isActionableUrl(url, options_.relativeLinks())) {
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
      case MD_SPAN_SUPERSCRIPT:
        openScripts_.push_back(1);
        break;
      case MD_SPAN_SUBSCRIPT:
        openScripts_.push_back(0);
        break;
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
        // A reference keeps only its own bit: a <sup> around it would raise a
        // footnote marker twice. It precedes any slice flush, so use the
        // stretch flush.
        const uint32_t refStyles = InlineStyle::FootnoteRef | (currentLink() >= 0 ? InlineStyle::Link : 0);
        appendStretchFlushed(asciiToUtf16(std::to_string(reference->id)), refStyles, currentLink());
        break;
      }
      default:
        // Other extensions are disabled by the parser flags.
        break;
    }
    return 0;
  }

  int leaveSpan(MD_SPANTYPE type, void * /*detail*/) {
    // Mirrors the enter gate: a span that never opened has no depth to give
    // back and no frame to close.
    if (inLiteralTag()) {
      return 0;
    }
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
      case MD_SPAN_MARK:
        highlightDepth_--;
        break;
      case MD_SPAN_U:
        underlineDepth_--;
        break;
      case MD_SPAN_SPOILER:
        spoilerDepth_--;
        break;
      case MD_SPAN_CODE:
        codeDepth_--;
        break;
      case MD_SPAN_LATEXMATH:
      case MD_SPAN_LATEXMATH_DISPLAY:
        mathDepth_--;
        break;
      case MD_SPAN_SUPERSCRIPT:
        removeScript(1);
        break;
      case MD_SPAN_SUBSCRIPT:
        removeScript(0);
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

    const bool verbatim = type == MD_TEXT_LATEXMATH;
    const uint32_t before = static_cast<uint32_t>(current_ ? current_->text.size() : 0);
    auto done = [&]() {
      noteBlockText(before, static_cast<uint32_t>(current_->text.size()));
      return 0;
    };

    switch (type) {
      case MD_TEXT_BR:
        stretchHard_++;
        stretchStyles_ = currentStyles();
        return done();
      case MD_TEXT_SOFTBR:
        // The fixture contract keeps source line breaks visible, but a stretch
        // that also holds a <br> or a hard break is counted once per hard mark.
        stretchSoft_++;
        stretchStyles_ = currentStyles();
        return done();
      case MD_TEXT_HTML: {
        const std::string_view html(text, size);
        if (options_.inlineHtml() && current_->kind != BlockKind::HtmlBlock) {
          const scan::TagPiece piece = scan::classifyTag(html, options_.tags);
          if (dropDepth_ > 0) {
            // Inside a dropped element: only its own closing tag matters, and
            // everything between — text, entities, nested tags — goes with it.
            if (piece.action == TagAction::DropContent && piece.closing && piece.name == dropName_) {
              dropDepth_--;
            }
            return done();
          }
          if (piece.comment) {
            // A comment emits nothing. Not a break and not text: a reader who
            // wrote `<!-- note -->` in a message means nothing by it, and
            // showing it is how a note to the renderer reaches the screen.
            return done();
          }
          if (piece.action == TagAction::DropContent) {
            dropName_ = piece.name;
            if (!piece.closing && !piece.selfClosing) {
              dropDepth_ = 1;
            }
            return done();
          }
          if (piece.action == TagAction::Hide || piece.action == TagAction::Unwrap) {
            // The tag is hidden and its content kept, which is what Streamdown's
            // sanitizer does for every element it has no style for.
            return done();
          }
          if (options_.tags.literalTagContent && literalDepth_ > 0) {
            // Inside a literal tag every piece is raw source: consumed, never
            // applied — no style, no link, no script. An allowed tag is the
            // exception, handled exactly as outside: recorded and pushed on
            // open, popped on close, so its record, its runs and the depth
            // balance the same way in both modes.
            if (piece.tag >= 0 && !piece.selfClosing) {
              scan::TagPiece opened = piece;
              if (!piece.closing) {
                opened.tag = recordAllowedTag(piece, htmlOffset(text));
                literalDepth_++;
              } else {
                literalDepth_--;
              }
              styleTagPush(opened);
            }
            return done();
          }
          switch (piece.action) {
            case TagAction::Style:
              switch (piece.style) {
                case TagStyle::Break:
                  trimTrailingSpaces();
                  stretchTag_++;
                  stretchStyles_ = currentStyles();
                  dropNextSpace_ = true;
                  return done();
                case TagStyle::Subscript:
                  if (piece.selfClosing) {
                    return done();
                  }
                  if (piece.closing) {
                    removeScript(0);
                  } else {
                    openScripts_.push_back(0);
                  }
                  return done();
                case TagStyle::Superscript:
                  if (piece.selfClosing) {
                    return done();
                  }
                  if (piece.closing) {
                    removeScript(1);
                  } else {
                    openScripts_.push_back(1);
                  }
                  return done();
                case TagStyle::VoidScript:
                  // `<sub/>` means nothing: nobody writes it to mean a script.
                  return done();
                default: {
                  // An app's own tag becomes a document record here, when it
                  // opens, so a run can name it and a press can read it. The
                  // policy's index is replaced by the document's on the way in.
                  scan::TagPiece opened = piece;
                  if (piece.tag >= 0 && !piece.closing && !piece.selfClosing) {
                    opened.tag = recordAllowedTag(piece, htmlOffset(text));
                    // From here to its close the content is raw source, when
                    // the app asked for it: the depth is what the span gates
                    // read, and the close brings it back down.
                    if (options_.tags.literalTagContent) {
                      literalDepth_++;
                    }
                  }
                  if (styleTagPush(opened)) {
                    return done();
                  }
                  break;
                }
              }
              break;
            case TagAction::Link:
              if (applyAnchorTag(piece, htmlOffset(text))) {
                return done();
              }
              break;
            case TagAction::Unwrap:
            case TagAction::DropContent:
            case TagAction::Hide:
            case TagAction::Literal:
              break;

          }
        }
        // Any other tag, and every tag while inline HTML is off, stays text.
        appendUtf8Text(std::string_view(text, size), currentStyles(), currentLink(), -1);
        return done();
      }
      case MD_TEXT_NULLCHAR: {
        std::u16string piece;
        piece.push_back(kReplacementCharacter);
        appendStretchFlushed(piece, currentStyles(), currentLink());
        return done();
      }
      case MD_TEXT_ENTITY: {
        std::u16string piece;
        appendEntity(piece, std::string_view(text, size));
        appendStretchFlushed(piece, currentStyles(), currentLink());
        return done();
      }
      case MD_TEXT_LATEXMATH:
        break;
      default: {
        if (dropDepth_ > 0) {
          return done();
        }
        std::string_view piece(text, size);
        if (dropNextSpace_) {
          size_t skip = 0;
          while (skip < piece.size() && (piece[skip] == ' ' || piece[skip] == '\t')) {
            skip++;
          }
          piece.remove_prefix(skip);
          if (piece.empty()) {
            // A whitespace-only piece between a <br> and the next text is
            // dropped: the break that will flush the stretch stands in its
            // place, and the count is not settled until real text arrives.
            return done();
          }
          dropNextSpace_ = false;
        }
        appendUtf8Text(piece, currentStyles(), currentLink(), currentTag());
        return done();
      }
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
      // when the app asked for it, and a constant two would read the character
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
      flushStretch(false);
      std::u16string piece;
      appendUtf8(piece, std::string_view(text, size));
      appendMath(piece, mathDisplay_);
      return done();
    }
    return done();
  }

  /// The direction of the last block finished, which a block with no strong
  /// character of its own inherits.
  BlockDirection lastDirection_{BlockDirection::LeftToRight};

  /// `openFence` says the source ended inside a fenced code block, which the
  /// parser cannot report on its own. Only the document's last block can be that
  /// fence, so the flag lands on it and nowhere else.
  DocumentHandle finish(bool openFence) {
    finishLeaf();
    if (openFence && !document_->blocks.empty()) {
      Block &last = document_->blocks.tail().back();
      if (last.kind == BlockKind::CodeBlock) {
        last.fenceOpen = true;
      }
    }
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

  /// One still-open style tag: what it sets, and which tag record it is when the
  /// app allowed it by name.
  struct StyleFrame {
    TagStyle style{TagStyle::None};
    int32_t tag{-1};
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
    /// This frame's tag's index into Document::tags, or -1. A Markdown link is
    /// -1; an `<a href>` names the tag it came from, which is what tells a
    /// writer's own anchor from a link the parser produced.
    int32_t tag{-1};
    /// True for a span md4c opened at a known source offset rather than one that
    /// started at a `[`. An `<a href>` is that: its text follows the `>`, so
    /// there is no bracket to scan back to and `spanRange` needs the piece's own
    /// offset as its anchor.
    bool tagOpen{false};
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

  /// One recognised line of an HTML block, or the text of a line that is not one.
  struct ContainerTagLine {
    enum class Kind : uint8_t {
      /// A `<details …>` opener, with the `open` attribute it carried.
      Open,
      /// A `</details>` closer.
      Close,
      /// A `<summary …>…</summary>` line, with the summary's text.
      Summary,
      /// Not a container tag. The line's text, delimiters included.
      Other,
      /// A tag the app allowed, used as a block container. `allowedTag` is its
      /// index in `ParseOptions::tags`, and the container it opens carries the
      /// document's own record of it.
      Allowed,
    };

    Kind kind{Kind::Other};
    bool open{false};
    int32_t allowedTag{-1};
    /// The tag's own name, lowercased. A closer carries it so it can be matched
    /// against the frame it is meant to end rather than against whatever frame
    /// happens to be innermost.
    std::u16string name;
    std::u16string summary;
    std::u16string text;
  };

  /// The index in `ParseOptions::tags` of `name`, or -1.
  ///
  /// A binary search because the policy is sorted by name, which is what makes it
  /// the same shape as the lookup `Scan.cpp` does for an inline tag — the block and
  /// inline paths read one list rather than two.
  int32_t policyIndexOf(std::u16string_view name) const {
    const auto found = std::lower_bound(
        options_.tags.tags.begin(), options_.tags.tags.end(), name, [](const AllowedTag &tag, std::u16string_view key) {
          return std::lexicographical_compare(
              tag.name.begin(), tag.name.end(), key.begin(), key.end(), [](char a, char16_t b) {
                return static_cast<unsigned char>(a) < b;
              });
        });
    if (found == options_.tags.tags.end()) {
      return -1;
    }
    // Compared through UTF-16 rather than by pointer: the policy's names are
    // `std::string` and the tag's is a slice of the source, so there is no
    // substring for a pointer comparison to find.
    return utf16ToUtf8(name) == found->name ? static_cast<int32_t>(found - options_.tags.tags.begin()) : -1;
  }

  /// Classifies one line of an HTML block.
  ///
  /// A tag counts only when the line is *nothing but* that tag, so a line of
  /// prose that merely mentions `<details>` stays the prose the writer wrote.
  ContainerTagLine classifyContainerLine(std::u16string_view line) {
    ContainerTagLine result;
    const std::u16string trimmed = trimAscii(line);
    if (trimmed.size() < 3 || trimmed.front() != u'<' || trimmed.back() != u'>') {
      result.kind = ContainerTagLine::Kind::Other;
      result.text = std::u16string(trimmed);
      return result;
    }
    // The name and the attributes, split at the first space. `<details-panel>`
    // therefore keeps its own name and is not this tag, which is what stops a
    // custom element of the same prefix from opening a container.
    size_t nameEnd = 1;
    while (nameEnd < trimmed.size() && trimmed[nameEnd] != u' ' && trimmed[nameEnd] != u'\t' &&
           trimmed[nameEnd] != u'>') {
      nameEnd++;
    }
    const std::u16string_view name(trimmed.data() + 1, nameEnd - 1);
    const bool closing = name.size() > 1 && name.front() == u'/';
    const std::u16string_view bare = closing ? name.substr(1) : name;
    // `<details/>` opens nothing and closes nothing, so it is a line of source.
    if (bare.size() > 1 && bare.back() == u'/') {
      result.kind = ContainerTagLine::Kind::Other;
      result.text = std::u16string(trimmed);
      return result;
    }
    if (bare == u"details" && !closing) {
      result.kind = ContainerTagLine::Kind::Open;
      // The rest is the attribute list, and `open` in it is what the writer's
      // starting state says.
      result.open = hasAttribute(
          std::u16string_view(trimmed.data() + nameEnd, trimmed.size() - nameEnd), u"open");
      return result;
    }
    // The app's own tags, used as *block* containers. A `<details>` is the same
    // kind of frame: the pair brackets the blocks between them, and the container
    // names the tag so a React child can be mounted in its place.
    if (!closing) {
      const int32_t allowed = policyIndexOf(bare);
      if (allowed >= 0) {
        result.kind = ContainerTagLine::Kind::Allowed;
        result.allowedTag = allowed;
        return result;
      }
    }
    if (closing && (bare == u"details" || policyIndexOf(bare) >= 0)) {
      result.kind = ContainerTagLine::Kind::Close;
      return result;
    }
    if (bare == u"summary" && !closing && endsWithAscii(trimmed, u"</summary>")) {
      // The summary's body is whatever sits between the two tags, which is what a
      // reader reads on the collapsed line. `nameEnd` is the opener's `>` and the
      // closer's own ten characters are excluded from the other end, so a nested
      // `</summary>` inside the body is not mistaken for this line's own closer.
      const size_t bodyEnd = trimmed.size() - 10;
      if (bodyEnd > nameEnd + 1) {
        result.kind = ContainerTagLine::Kind::Summary;
        result.summary = unwrapInlineTags(std::u16string_view(trimmed).substr(nameEnd + 1, bodyEnd - nameEnd - 1));
        return result;
      }
    }
    result.kind = ContainerTagLine::Kind::Other;
    result.text = std::u16string(trimmed);
    return result;
  }

  /**
   * True when the attribute list `rest` carries `name`.
   *
   * Presence is the whole question, because `open` is a boolean attribute: HTML
   * says a boolean attribute is true whenever it appears, whatever its value,
   * so `<details open>`, `<details open="open">` and `<details open="">` all mean
   * the same thing and only its absence means closed. `opened` is a different
   * attribute, which is why the name is compared whole rather than by prefix.
   */
  static bool hasAttribute(std::u16string_view rest, std::u16string_view name) {
    size_t at = 0;
    while (at < rest.size()) {
      while (at < rest.size() && isAttributeSpace(rest[at])) {
        at++;
      }
      if (at >= rest.size() || rest[at] == u'>') {
        return false;
      }
      const size_t start = at;
      while (at < rest.size() && rest[at] != u'=' && rest[at] != u'>' && !isAttributeSpace(rest[at])) {
        at++;
      }
      const bool matched = std::u16string_view(rest.data() + start, at - start) == name;
      if (matched) {
        return true;
      }
      // Step over the value, so a value that happens to contain the name — an
      // `id="open"` — is not read as the attribute itself.
      while (at < rest.size() && isAttributeSpace(rest[at])) {
        at++;
      }
      if (at < rest.size() && rest[at] == u'=') {
        at++;
        if (at < rest.size() && (rest[at] == u'"' || rest[at] == u'\'')) {
          const char16_t quote = rest[at];
          at++;
          while (at < rest.size() && rest[at] != quote) {
            at++;
          }
          at = at < rest.size() ? at + 1 : at;
        } else {
          while (at < rest.size() && rest[at] != u'>' && !isAttributeSpace(rest[at])) {
            at++;
          }
        }
      }
    }
    return false;
  }

  /// Whitespace between attributes, which is a space or a tab and nothing else.
  static bool isAttributeSpace(char16_t unit) { return unit == u' ' || unit == u'\t'; }

  /**
   * Turns one HTML block's lines into containers, a summary leaf, and whatever
   * text is left over.
   *
   * md4c gives an HTML block up to a blank line, so `<details>` and its
   * `<summary>` arrive in one block, the body arrives as ordinary blocks in
   * between, and `</details>` arrives in a block of its own. Pairing those is
   * what a stack of frames does, the same way a quote and a list item are frames:
   * the opener pushes one, the closer pops the nearest, and a block in between
   * finds itself inside by being built while the frame is open.
   *
   * Nesting works because the stack is a stack. A closer with no opener is
   * hidden rather than shown as source: a `</details>` a reader sees is a tag the
   * writer left behind, and Streamdown's sanitizer drops it too.
   */
  void consumeContainerHtml() {
    if (!current_ || current_->kind != BlockKind::HtmlBlock) {
      return;
    }
    // Every line is classified before anything is emitted, because emitting a
    // summary finishes the leaf being built — and the leaf being built is this
    // HTML block, whose text has to be the leftovers by then. Classifying first
    // is what makes the order of the two irrelevant.
    const std::u16string text = current_->text;
    std::vector<ContainerTagLine> lines;
    size_t at = 0;
    while (at <= text.size()) {
      const size_t newline = text.find(u'\n', at);
      lines.push_back(classifyContainerLine(newline == std::u16string::npos
                                                ? std::u16string_view(text).substr(at)
                                                : std::u16string_view(text).substr(at, newline - at)));
      if (newline == std::u16string::npos) {
        break;
      }
      at = newline + 1;
    }
    const bool recognised =
        std::any_of(lines.begin(), lines.end(), [](const ContainerTagLine &line) {
          return line.kind != ContainerTagLine::Kind::Other;
        });
    if (!recognised) {
      // Nothing in this block was a container tag, so the leaf keeps the text it
      // was built with and `promoteLeaf` promotes it as an ordinary HTML block.
      return;
    }
    // The lines are acted on in two passes, and the order is the whole trick.
    //
    // Emitting a summary finishes this very leaf, and this leaf is the HTML block
    // whose text has to be the leftovers by then — so every frame is pushed and
    // popped first, the text is written back, and only then is anything emitted.
    // A closer pops the innermost frame this parser opened, whichever tag named
    // it: HTML has no rule for a mismatched closer and CommonMark's own containers
    // behave the same way, so `</user>` inside a `<details>` ends the details.
    for (const ContainerTagLine &line : lines) {
      switch (line.kind) {
        case ContainerTagLine::Kind::Open:
          pushContainer(ContainerKind::Details, u"", TaskState::None);
          containers_.back().container.open = line.open;
          break;
        case ContainerTagLine::Kind::Allowed:
          pushCustom(line.allowedTag);
          break;
        case ContainerTagLine::Kind::Close:
          // A closer with nothing open is dropped, and so is the whole line: the
          // frame it belongs to is not here, and a reader cannot act on a tag.
          (void)popFrame();
          break;
        case ContainerTagLine::Kind::Summary:
        case ContainerTagLine::Kind::Other:
          break;
      }
    }

    std::u16string leftover;
    for (const ContainerTagLine &line : lines) {
      if (line.kind != ContainerTagLine::Kind::Other || line.text.empty()) {
        continue;
      }
      if (!leftover.empty()) {
        leftover.push_back(u'\n');
      }
      leftover.append(line.text);
    }
    current_->text = leftover;

    for (const ContainerTagLine &line : lines) {
      if (line.kind == ContainerTagLine::Kind::Summary) {
        emitSummary(line.summary);
      }
    }
  }

  /// Opens the frame for a block tag the app allowed.
  ///
  /// The container names the tag by its index in the document's own tag list, so
  /// a React child that replaces the block knows which tag it is standing in and
  /// the reader's press on the tag maps back to where it was written. The record
  /// is made here, at the opener, because that is the one point in the parse at
  /// which the whole tag is known.
  void pushCustom(int32_t policyIndex) {
    const int32_t tag = recordAllowedTagAt(policyIndex, leafStart_ == kUnknownOffset ? source_.size() : leafStart_);
    pushContainer(ContainerKind::Custom, u"", TaskState::None);
    containers_.back().container.tag = tag;
  }

  /// A document record for one of the app's own tags, by its policy index.
  int32_t recordAllowedTagAt(int32_t policyIndex, size_t at) {
    if (policyIndex < 0 || static_cast<size_t>(policyIndex) >= options_.tags.tags.size()) {
      return -1;
    }
    const AllowedTag &allowed = options_.tags.tags[policyIndex];
    Tag record;
    record.name = allowed.name;
    record.block = true;
    record.range.start = offsets_.toUtf16(at);
    document_->tags.push_back(std::move(record));
    return static_cast<int32_t>(document_->tags.size() - 1);
  }

  /// Pops the nearest frame this parser opened, if there is one.
  ///
  /// Either kind: a `<details>` and an app's own block tag are both containers,
  /// and a closer for one inside the other closes the inner one. Returns whether
  /// it did, which is what lets a stray closer be dropped rather than shown.
  bool popFrame() {
    for (auto frame = containers_.rbegin(); frame != containers_.rend(); ++frame) {
      if (frame->container.kind == ContainerKind::Details || frame->container.kind == ContainerKind::Custom) {
        containers_.erase(std::next(frame).base());
        return true;
      }
    }
    return false;
  }

  /// True while a `<details>` frame is open, which is what a summary needs.
  bool insideDetails() const {
    for (const auto &frame : containers_) {
      if (frame.container.kind == ContainerKind::Details) {
        return true;
      }
    }
    return false;
  }

  /**
   * Emits the block a `<summary>` names.
   *
   * A synthetic leaf inside the container, the way an admonition's title is one:
   * it is not in the source as a block, so it cannot be one, and a platform
   * draws the chevron and the pressed state from the container frame rather than
   * from text the writer never typed.
   *
   * `showsMarker` on the frame is the hook: it is what the list-item marker
   * already uses to say "the first leaf of this container draws the chrome", and
   * a collapsible block's summary is exactly that leaf.
   */
  void emitSummary(const std::u16string &summary) {
    if (!insideDetails()) {
      return;
    }
    // The frame's marker is what says "this leaf is the container's own", and
    // `beginLeaf` consumes it as it copies the frame, so it is set before the
    // leaf rather than after: set afterwards, the body's first leaf would claim
    // the summary's chevron and the summary itself would read as an ordinary line.
    for (auto frame = containers_.rbegin(); frame != containers_.rend(); ++frame) {
      if (frame->container.kind == ContainerKind::Details) {
        frame->markerPending = true;
        break;
      }
    }
    beginLeaf(BlockKind::Paragraph);
    appendText(summary, InlineStyle::Strong, -1);
    finishLeaf();
  }

  /**
   * A summary's own text, with the tags inside it unwrapped.
   *
   * `<summary>a <code>b</code></summary>` has to read as the summary a writer
   * meant rather than as the markup they typed, so the tags go and their content
   * stays — the rule the table gives every element it has no drawing for. The
   * summary is a synthetic leaf rather than a parsed span, so a style the writer
   * used inside it does not become a run; the text is what a screen reader needs
   * and what the summary's own strong run already draws.
   */
  static std::u16string unwrapInlineTags(std::u16string_view body) {
    std::u16string out;
    size_t at = 0;
    while (at < body.size()) {
      const size_t open = body.find(u'<', at);
      if (open == std::u16string_view::npos) {
        out.append(body.substr(at));
        break;
      }
      out.append(body.substr(at, open - at));
      const size_t close = body.find(u'>', open);
      if (close == std::u16string_view::npos) {
        // An unterminated `<` is text, which is what a reader who typed one meant.
        out.append(body.substr(open));
        break;
      }
      const std::u16string_view tag = body.substr(open, close - open + 1);
      // A comment is dropped whole; every other tag keeps what it wrapped.
      if (tag.size() < 4 || tag.compare(0, 4, u"<!--") != 0) {
        const bool closing = tag.size() > 2 && tag[1] == u'/';
        const bool opening = tag.size() > 2 && tag[2] != u'/';
        if (!closing && !opening) {
          at = close + 1;
          continue;
        }
      }
      at = close + 1;
    }
    return out;
  }

  static std::u16string trimAscii(std::u16string_view text) {
    size_t start = 0;
    size_t end = text.size();
    while (start < end && (text[start] == u' ' || text[start] == u'\t')) {
      start++;
    }
    while (end > start && (text[end - 1] == u' ' || text[end - 1] == u'\t' || text[end - 1] == u'\r')) {
      end--;
    }
    return std::u16string(text.substr(start, end - start));
  }

  static bool endsWithAscii(std::u16string_view text, std::u16string_view suffix) {
    return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
  }

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
    // A style tag runs to the end of its block, so it is cleared with
    // the scripts it sits beside.
    tagStyles_.clear();
    tagIndexes_.clear();
        openScripts_.clear();
    stretchSoft_ = stretchHard_ = stretchTag_ = 0;
    stretchStyles_ = 0;
    dropNextSpace_ = false;
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
    flushStretch(true);
    // A style tag runs to the end of its block, so it is cleared with
    // the scripts it sits beside.
    tagStyles_.clear();
    tagIndexes_.clear();
        openScripts_.clear();
    dropNextSpace_ = false;
    // The block's layout direction, decided here because this is the last moment
    // its text is complete.
    //
    // A block the rule leaves alone — code, math, or one with no strong character
    // of its own — takes the direction already in force rather than deciding
    // again, so it neither reads the other way nor changes what the block after it
    // inherits. That is also why `direction` is never `Inherit` here: the
    // platforms draw, they do not decide.
    const BlockDirection direction = blockDirection(*current_, lastDirection_);
    if (direction != BlockDirection::Inherit) {
      lastDirection_ = direction;
    }
    current_->direction = lastDirection_;
    promoteLeaf();
    // An HTML block the container rules consumed whole emits nothing.
    //
    // The block is *still* an HTML block here, and that is what distinguishes it
    // from one whose pieces were all hidden: that one was promoted to a paragraph
    // on the way through `promoteLeaf`, and an empty paragraph is a line of space
    // a comment-only block has always taken. A `<details>` tag pair is not a line
    // of prose, so it gets no block at all.
    if (current_->kind != BlockKind::HtmlBlock || !current_->text.empty()) {
      document_->blocks.push_back(std::move(*current_));
    }
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
    if (current_->kind == BlockKind::HtmlBlock) {
      // The `<br>` rule is pre-existing and unconditional. The comment and the
      // dropped-element rule is part of the HTML subset, so it is gated with it:
      // `inlineHtml: false` is a view that wants every tag as source, and a
      // comment is a tag.
      promoteBrOnlyHtml() || (options_.inlineHtml() && promoteHiddenHtml());
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

  /// A block that is nothing but `<br>` tags and whitespace becomes a
  /// paragraph of empty lines, one per tag: a paragraph holding n−1 newlines
  /// measures one line per tag, as does the mock. Anything else keeps the
  /// html block's raw, visible text.
  bool promoteBrOnlyHtml() {
    const std::u16string &text = current_->text;
    size_t count = 0;
    size_t i = 0;
    while (i < text.size()) {
      while (i < text.size() && (text[i] == u' ' || text[i] == u'\t' || text[i] == u'\r' || text[i] == u'\n')) {
        i++;
      }
      if (i >= text.size()) {
        break;
      }
      size_t j = i;
      while (j < text.size() && !(text[j] == u' ' || text[j] == u'\t' || text[j] == u'\r' || text[j] == u'\n')) {
        j++;
      }
      std::string ascii;
      for (size_t k = i; k < j; k++) {
        if (text[k] >= 0x80) {
          return false;
        }
        ascii.push_back(static_cast<char>(text[k]));
      }
      if (scan::classifyTag(ascii, options_.tags).style != TagStyle::Break) {
        return false;
      }
      count++;
      i = j;
    }
    if (count == 0) {
      return false;
    }
    current_->kind = BlockKind::Paragraph;
    current_->text.assign(count - 1, u'\n');
    current_->runs.clear();
    return true;
  }

  /**
   * An HTML block that holds nothing a reader should see becomes no block at
   * all.
   *
   * Three shapes, and they are the three ways a writer leaves something in a
   * message that is not prose: a comment, a dropped element with its content,
   * and whitespace. A block of them is what md4c produces for a line of
   * `<!-- note -->` or `<script>…</script>`, and drawing it would show a reader
   * the source of a tag the engine already decided to hide.
   *
   * The block is emptied rather than removed, because the incremental parser
   * counts blocks and the segment plan indexes them; an empty paragraph is one
   * that draws nothing and is one row in every list.
   */
  bool promoteHiddenHtml() {
    const std::u16string &text = current_->text;
    size_t i = 0;
    size_t pieces = 0;
    while (i < text.size()) {
      while (i < text.size() && isSpaceUnit(text[i])) {
        i++;
      }
      if (i >= text.size()) {
        break;
      }
      // A comment is opaque: its body is not a word, so the scan steps over the
      // whole run to its `-->` rather than splitting on the space inside it.
      if (text.compare(i, 4, u"<!--") == 0) {
        size_t end = i + 4;
        while (end + 3 <= text.size() && text.compare(end, 3, u"-->") != 0) {
          end++;
        }
        // An unclosed `<!--` is not a comment: hiding to the end of the block
        // would hide prose a writer typed after a stray opener, which is the one
        // way this rule could lose a reader's text.
        if (end + 3 > text.size()) {
          return false;
        }
        const size_t after = end + 3;
        pieces++;
        i = after;
        continue;
      }
      size_t j = i;
      while (j < text.size() && !isSpaceUnit(text[j])) {
        j++;
      }
      std::string ascii;
      for (size_t k = i; k < j; k++) {
        if (text[k] >= 0x80) {
          return false;
        }
        ascii.push_back(static_cast<char>(text[k]));
      }
      if (scan::classifyTag(ascii, options_.tags).action != TagAction::DropContent) {
        return false;
      }
      pieces++;
      i = j;
    }
    if (pieces == 0) {
      return false;
    }
    current_->kind = BlockKind::Paragraph;
    current_->text.clear();
    current_->runs.clear();
    return true;
  }

  static bool isSpaceUnit(char16_t unit) {
    return unit == u' ' || unit == u'\t' || unit == u'\r' || unit == u'\n';
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
    // A stretch that ends in a cell is one break shorter, as at a block end.
    flushStretch(true);
    promoteLeaf();
    // A style tag runs to the end of its block, so it is cleared with
    // the scripts it sits beside.
    tagStyles_.clear();
    tagIndexes_.clear();
        openScripts_.clear();
    dropNextSpace_ = false;
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
    if (highlightDepth_ > 0) {
      styles |= InlineStyle::Highlight;
    }
    if (underlineDepth_ > 0) {
      styles |= InlineStyle::Underline;
    }
    if (spoilerDepth_ > 0) {
      styles |= InlineStyle::Spoiler;
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
    if (!openScripts_.empty()) {
      // The innermost still-open script wins, so <sup>a<sub>b</sub></sup>
      // lowers b. Nesting the same tag twice sets the same bit twice.
      styles |= openScripts_.back() == 0 ? InlineStyle::Subscript : InlineStyle::Superscript;
    }
    // A tag's own style, from every still-open one. Like the scripts, nesting
    // the same tag twice sets the same bit twice rather than shifting further,
    // and a `<mark>` inside a `<u>` is both.
    for (const StyleFrame &frame : tagStyles_) {
      styles |= styleBit(frame.style);
    }
    return styles;
  }

  /// The tag index a run is attributed to, or -1.
  ///
  /// The innermost still-open app-allowed tag, which is the one a press means.
  /// A Markdown link, a code span and a formula all answer -1 unless an allowed
  /// tag is open around them.
  int32_t currentTag() const { return tagIndexes_.empty() ? -1 : tagIndexes_.back(); }

  /// The style bit a tag style sets, which is 0 for the two that are not styles.
  static uint32_t styleBit(TagStyle style) {
    switch (style) {
      case TagStyle::Strong:
        return InlineStyle::Strong;
      case TagStyle::Emphasis:
        return InlineStyle::Emphasis;
      case TagStyle::Strikethrough:
        return InlineStyle::Strikethrough;
      case TagStyle::Code:
        return InlineStyle::Code;
      case TagStyle::Keyboard:
        // A key cap is inline code with a border, so the bit is the same one and
        // the platform reads the border off the theme slot it also draws.
        return InlineStyle::Keyboard;
      case TagStyle::Highlight:
        return InlineStyle::Highlight;
      case TagStyle::Underline:
        return InlineStyle::Underline;
      default:
        return 0;
    }
  }

  /**
   * Opens or closes a style the table named.
   *
   * A stack rather than a depth, for the reason the scripts have one: the
   * innermost open tag wins and nesting the same tag twice does not shift
   * further. `<mark>a<b>b</b>c</mark>` is all three styles on `b`.
   *
   * A self-closed tag opens nothing, and a stray end tag closes nothing — both
   * are what an HTML parser does, and both are what keeps a model's unbalanced
   * `<u>` from colouring the rest of a message.
   */
  bool styleTagPush(const scan::TagPiece &piece) {
    if (piece.style == TagStyle::None && piece.tag < 0) {
      // Neither a style of ours nor an app's own tag: nothing to open.
      return false;
    }
    if (piece.selfClosing) {
      // A self-closed style tag styles nothing, which is why `VoidScript` only
      // reaches here for the two names a browser voids. An app's own tag is the
      // exception: the tag still exists and can still be pressed, it just wraps
      // nothing, which is what `<user/>` means.
      return piece.style == TagStyle::VoidScript || piece.tag >= 0;
    }
    if (!piece.closing) {
      tagStyles_.push_back({piece.style, piece.tag});
      // An allowed tag with no style still contributes its index, which is the
      // whole point of allowing one: `onTagPress` and `markdownStyle.tags` need
      // something to name.
      if (piece.tag >= 0) {
        tagIndexes_.push_back(piece.tag);
      }
      return true;
    }
    for (auto it = tagStyles_.rbegin(); it != tagStyles_.rend(); ++it) {
      // Matched on the index when there is one, so an app's own tag closes its
      // own frame rather than whichever frame happens to carry no style.
      if (piece.tag >= 0 ? it->tag != piece.tag : it->style != piece.style) {
        continue;
      }
      if (it->tag >= 0) {
        // The innermost still-open record of this tag's index comes off with it.
        for (auto jt = tagIndexes_.rbegin(); jt != tagIndexes_.rend(); ++jt) {
          if (*jt == it->tag) {
            tagIndexes_.erase(std::next(jt).base());
            break;
          }
        }
      }
      tagStyles_.erase(std::next(it).base());
      return true;
    }
    return true;
  }

  /**
   * The document record for an app-allowed tag, appended as the tag opens.
   *
   * One record per opening tag rather than one per name, because a document that
   * uses `<user>` twice has two occurrences and a press on the second must point
   * at the second. `at` is the piece's own offset, so the record's range is where
   * the writer wrote it.
   */
  int32_t recordAllowedTag(const scan::TagPiece &piece, size_t at) {
    Tag record;
    record.name = piece.name;
    record.attributes = piece.attributes;
    record.range.start = offsets_.toUtf16(at);
    document_->tags.push_back(std::move(record));
    const auto index = static_cast<int32_t>(document_->tags.size() - 1);
    // The range runs to the closing tag, which md4c has not reported yet; it is
    // completed when the tag closes, and an unclosed one keeps its start, which
    // is what a reader can still be pointed at.
    pendingTagRanges_.emplace_back(index, at);
    return index;
  }

  /**
   * Applies an `<a href>` the same way a Markdown link is applied.
   *
   * The URL goes through the same policy, so `<a href="javascript:x">` stays
   * text and `<a href="tel:+1">` is a link, and it is entity-decoded the same
   * way, so `href="?a=1&amp;b=2"` is the URL a reader means. `at` is the piece's
   * own offset in the source, which is where the record's range comes from and
   * what makes a press on an anchor mappable back to where it was written.
   */
  bool applyAnchorTag(const scan::TagPiece &piece, size_t at) {
    if (piece.closing) {
      // The frame is popped here rather than in a leave-span callback, because
      // md4c reports `<a href>` as inline HTML and so opens no span of its own:
      // the text between the tags is ordinary text, and `</a>` is the only thing
      // that says the anchor ended.
      closeLinkOrImage();
      return true;
    }
    {
      if (piece.selfClosing) {
        return true;
      }
      SpanFrame frame;
      frame.image = false;
      frame.cursorAtEnter = cursor_;
      // The piece itself bounds the anchor even before any text lands in it, so
      // `spanRange` can recover `[open … )]` from a forward scan alone.
      frame.firstText = at;
      frame.lastTextEnd = at;
      frame.tagOpen = true;
      const std::string url = decodeAttribute(piece.href);
      if (isActionableUrl(url, options_.relativeLinks())) {
        Tag record;
        record.name = "a";
        document_->tags.push_back(std::move(record));
        frame.tag = static_cast<int32_t>(document_->tags.size() - 1);
        frame.link = static_cast<int32_t>(document_->links.size());
        document_->links.push_back({url, {}, frame.tag});
      }
      spans_.push_back(frame);
      return true;
    }
  }

  /**
   * An attribute value with its entities decoded.
   *
   * `appendEntity` rather than a straight copy, so `&amp;` in a query string is
   * one `&` — the same answer `attributeToUtf8` gives an `MD_ATTRIBUTE`, and the
   * two must not differ, or one spelling of a URL would not be a link.
   */
  std::string decodeAttribute(std::string_view raw) const {
    std::u16string decoded;
    size_t i = 0;
    while (i < raw.size()) {
      if (raw[i] != '&') {
        appendUtf8(decoded, std::string_view(raw).substr(i, 1));
        i++;
        continue;
      }
      const size_t semicolon = raw.find(';', i);
      // A bare `&` is a bare `&`, which is what a browser reads too: only a
      // named or numeric entity ends in a semicolon.
      if (semicolon == std::string_view::npos || semicolon == i + 1) {
        appendUtf8(decoded, std::string_view(raw).substr(i, 1));
        i++;
        continue;
      }
      appendEntity(decoded, raw.substr(i, semicolon - i + 1));
      i = semicolon + 1;
    }
    return utf16ToUtf8(decoded);
  }

  int32_t currentLink() const {
    for (auto it = spans_.rbegin(); it != spans_.rend(); ++it) {
      if (!it->image) {
        return it->link;
      }
    }
    return -1;
  }

  void appendText(const std::u16string &piece, uint32_t styles, int32_t link, int32_t tag = -1) {
    if (piece.empty()) {
      return;
    }
    auto &block = *current_;
    const auto start = static_cast<uint32_t>(block.text.size());
    block.text.append(piece);
    recordRun(start, styles, link, tag);
  }

  // Decode directly into the leaf instead of allocating and copying a UTF-16
  // string for every md4c callback, including every fragment between tags.
  void appendUtf8Text(std::string_view piece, uint32_t styles, int32_t link, int32_t tag = -1) {
    flushStretch(false);
    const auto start = static_cast<uint32_t>(current_->text.size());
    appendUtf8(current_->text, piece);
    recordRun(start, styles, link, tag);
  }

  void recordRun(uint32_t start, uint32_t styles, int32_t link, int32_t tag = -1) {
    auto &block = *current_;
    const auto end = static_cast<uint32_t>(block.text.size());
    if (start == end || (styles == 0 && link < 0 && tag < 0)) {
      return;
    }
    if (!block.runs.empty()) {
      auto &last = block.runs.back();
      if (last.end == start && last.styles == styles && last.link == link && last.tag == tag) {
        last.end = end;
        return;
      }
    }
    block.runs.push_back({start, end, styles, link, tag});
  }

  /// The UTF-8 offset of an `MD_TEXT_HTML` piece, read off md4c's pointer.
  ///
  /// `kUnknownOffset` when the pointer is not into `parse_`, which cannot happen
  /// for a piece md4c just reported but is cheaper to prove than to assume.
  size_t htmlOffset(const MD_CHAR *text) const {
    if (text < parse_.data() || text >= parse_.data() + parse_.size()) {
      return kUnknownOffset;
    }
    return static_cast<size_t>(text - parse_.data());
  }

  /// Folds the pending stretch into the block text with the per-stretch rule:
  /// every tag and every hard break counts one line break, and soft breaks
  /// count only when the stretch held no tag and no hard break. ‘trimmed’
  /// is the block- or cell-end rule: a final break is invisible, so a
  /// stretch-ending count is one smaller.
  void flushStretch(bool trimmed) {
    if (stretchSoft_ == 0 && stretchHard_ == 0 && stretchTag_ == 0) {
      return;
    }
    int n = (stretchTag_ + stretchHard_) > 0 ? stretchTag_ + stretchHard_ : stretchSoft_;
    if (trimmed) {
      n = std::max(0, n - 1);
    }
    if (n > 0) {
      appendText(std::u16string(n, u'\n'), stretchStyles_, currentLink());
    }
    stretchSoft_ = stretchHard_ = stretchTag_ = 0;
    dropNextSpace_ = false;
  }

  /// Flushes the pending stretch before appending a piece of real text, so a
  /// newline run always stands between the breaks that caused it and the text
  /// that ends them.
  void appendStretchFlushed(const std::u16string &piece, uint32_t styles, int32_t link) {
    flushStretch(false);
    appendText(piece, styles, link);
  }

  /// Trims spaces and tabs left of a <br>, which HTML's whitespace collapsing
  /// drops the way the builder does it; run ends are clipped, never extended.
  void trimTrailingSpaces() {
    if (!current_) {
      return;
    }
    auto &text = current_->text;
    size_t n = text.size();
    while (n > 0 && (text[n - 1] == u' ' || text[n - 1] == u'\t')) {
      n--;
    }
    if (n == text.size()) {
      return;
    }
    text.resize(n);
    // Runs are sorted and disjoint. Only the suffix can overlap the trimmed
    // whitespace; visiting the prefix for every <br> makes a rich leaf quadratic.
    auto &runs = current_->runs;
    while (!runs.empty() && runs.back().start >= n) {
      runs.pop_back();
    }
    if (!runs.empty()) {
      runs.back().end = std::min<uint32_t>(runs.back().end, static_cast<uint32_t>(n));
    }
  }

  /// Removes the last open script of the kind /kind/, or no-op when none is
  /// open: a stray end tag is a browser's no-op too, and an escaped opener
  /// leaves no open to close.
  void removeScript(int kind) {
    for (auto it = openScripts_.rbegin(); it != openScripts_.rend(); ++it) {
      if (*it == static_cast<uint8_t>(kind)) {
        openScripts_.erase(std::next(it).base());
        return;
      }
    }
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
          if (!isUnfinishedDestination(begin) && isActionableUrl(span.autolinkUrl, options_.relativeLinks()) &&
              autolinkAllowed(span.autolinkUrl)) {
            span.link = static_cast<int32_t>(document_->links.size());
            document_->links.push_back({std::move(span.autolinkUrl), {}});
          }
        }
      }
      span.lastTextEnd = span.lastTextEnd == kUnknownOffset ? end : std::max(span.lastTextEnd, end);
    }
    cursor_ = end;
  }

  /**
   * Whether an autolink's protocol survived the three `disableAutolinkProtocols`
   * bits.
   *
   * Streamdown normalizes a configured protocol before comparing it: trimmed,
   * lower cased, and with the colon optional. This reads the protocol the same
   * way, because `mailto` and `MAILTO:` are the same answer to "is this
   * disabled" and a caller who wrote one of them must not get the other.
   *
   * A `www.` autolink is judged by `http`, because that is the scheme md4c gave
   * it, and a `tel:`-or-relative one never reaches here: md4c autolinks only
   * `http`, `https` and bare addresses.
   */
  bool autolinkAllowed(std::string_view url) const {
    const bool mailto = options_.autolinksMailto();
    const bool http = options_.autolinksHttp();
    const bool https = options_.autolinksHttps();
    if (mailto && http && https) {
      return true;
    }
    size_t schemeEnd = url.find(':');
    if (schemeEnd == std::string_view::npos) {
      return true;
    }
    std::string scheme(url.substr(0, schemeEnd));
    for (char &c : scheme) {
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (scheme == "www") {
      return http;
    }
    if (scheme == "http" || scheme == "https") {
      return scheme == "http" ? http : https;
    }
    if (scheme == "mailto") {
      return mailto;
    }
    return true;
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

    if (span.tagOpen) {
      // An `<a href>` opened at its own `<`, so the range starts there and ends
      // after the matching `>` plus the text it wraps. `lastTextEnd` is the end
      // of the last text inside the span, which is the `</a>`'s own offset when
      // md4c reports it, and the block's otherwise.
      if (anchor == kUnknownOffset) {
        return std::nullopt;
      }
      size_t end = span.lastTextEnd == kUnknownOffset ? anchor : span.lastTextEnd;
      // md4c reports the closing tag's own text, so step over it.
      if (end < source_.size() && source_[end] == '<') {
        const size_t close = source_.find('>', end);
        if (close != std::string_view::npos) {
          end = close + 1;
        }
      }
      return std::make_pair(anchor, end);
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
      if (span.tag >= 0) {
        // The tag's own range is the anchor's, so `onTagPress` and a press on the
        // link agree about where in the source either one happened.
        Tag &tag = document_->tags.tail()[static_cast<size_t>(span.tag)];
        tag.range = link.range;
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
  ParseOptions options_;
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
  int highlightDepth_{0};
  int underlineDepth_{0};
  int spoilerDepth_{0};
  int codeDepth_{0};
  /// Still-open style tags from inline HTML, outermost first. Cleared with the
  /// block, because an unclosed `<mark>` runs to the end of its block and not
  /// past it — the same rule a browser applies inside a block element.
  std::vector<StyleFrame> tagStyles_;
  /// The indices of the still-open tags the app allowed, so a run can name the
  /// innermost one. A subset of `tagStyles_`, kept beside it rather than inside
  /// it because only an allowed tag has an index.
  std::vector<int32_t> tagIndexes_;
  /// An allowed tag that opened and has not closed, with the offset it opened
  /// at, so its record's range can be completed when it does.
  std::vector<std::pair<int32_t, size_t>> pendingTagRanges_;

  /// Completes an allowed tag's range from the offset its closing piece is at.
  void closeTagRange(int32_t tag) {
    for (size_t i = 0; i < pendingTagRanges_.size(); i++) {
      if (pendingTagRanges_[i].first != tag) {
        continue;
      }
      const size_t start = pendingTagRanges_[i].second;
      pendingTagRanges_.erase(pendingTagRanges_.begin() + static_cast<ptrdiff_t>(i));
      Tag &record = document_->tags.tail()[static_cast<size_t>(tag)];
      const size_t close = source_.find('>', start);
      record.range.end = offsets_.toUtf16(close == std::string_view::npos ? start : close + 1);
      return;
    }
  }

  /// How deep inside a dropped element the block is: 0 outside one.
  ///
  /// An unclosed `<script>` drops the rest of its block and nothing after it,
  /// which is what a browser does inside a block element, and is the safe
  /// direction: the content was going to be dropped anyway.
  unsigned dropDepth_{0};
  /// The dropped element's name, so a nested `</div>` cannot end it.
  std::string_view dropName_;
  /// How deep inside an app-allowed tag the block is: 0 outside one.
  ///
  /// Only counts while `literalTagContent` is on, and only within its block,
  /// because the policy promises the raw source between an open and a close in
  /// one block. The span gates read it rather than the flag alone, so an
  /// allowed tag nests and unclosed ones end with their paragraph.
  unsigned literalDepth_{0};
  /// True while inside an app-allowed tag whose content is raw source.
  bool inLiteralTag() const { return options_.tags.literalTagContent && literalDepth_ > 0; }
  /// Open script tags, innermost last: 0 is a subscript, 1 a superscript.
  /// A small stack, not two depths, because the innermost-opened one wins and
  /// nesting the same tag twice does not shift further.
  std::vector<uint8_t> openScripts_;
  /// A run of soft breaks, hard breaks and <br> tags with no text between
  /// them: each tag and each hard break counts one '\n', and soft breaks
  /// count only when the stretch holds no tag and no hard break. Flushed once,
  /// when the next text arrives or the block or cell ends.
  int stretchSoft_{0};
  int stretchHard_{0};
  int stretchTag_{0};
  uint32_t stretchStyles_{0};
  /// Spaces directly around a <br> are dropped, as HTML's collapsing does.
  bool dropNextSpace_{false};
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

ParseResult parseMarkdown(std::string_view source, ParseOptions options) {
  ParseResult result;
  result.parse = options;
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
    result.document = builder.finish(false);
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
  if (options.superscriptSyntax()) {
    parser.flags |= MD_FLAG_SUPERSCRIPTS;
  }
  if (options.subscriptSyntax()) {
    parser.flags |= MD_FLAG_SUBSCRIPTS;
  }
  // md4c's own three: `==mark==`, `__underline__` and `||spoiler||`. Each behind
  // its own bit because each changes what the delimiters mean everywhere — and
  // `__underline__` in particular takes `_` away from emphasis, so it is never on
  // unless the app asked for it by name.
  if (options.highlightSyntax()) {
    parser.flags |= MD_FLAG_HIGHLIGHT;
  }
  if (options.underlineSyntax()) {
    parser.flags |= MD_FLAG_UNDERLINE;
  }
  if (options.spoilerSyntax()) {
    parser.flags |= MD_FLAG_SPOILERS;
  }
  if (options.cjk()) {
    parser.flags |= MD_FLAG_CJKFRIENDLY;
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
  result.document = builder.finish(scan::endsInsideFence(source));
  result.containerCount = builder.containerCount();
  result.tableCount = builder.tableCount();
  result.blockSource = builder.takeBlockSource();
  return result;
}

ParseResult parseMarkdownAt(std::string_view region, size_t origin, ParseOptions options) {
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
