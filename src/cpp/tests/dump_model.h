// The render model as JSON, shared by the host test drivers so every one of
// them describes a document the same way. `tests/engine.ts` reads this
// shape, and a test that asserts on the text a reader would see can only do
// that if the drivers agree on what a field means.

#pragma once

#include "Document.h"
#include "FenceSlots.h"
#include "MarkdownParser.h"
#include "Unicode.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace downstream::testdump {

/// The name a token class is reported under. A platform palette keys on the
/// class, so the name is the contract the golden fixtures pin.
/// The delimiter a formula was written with, which is what a test asserts on
/// when it has to tell a promoted `$$…$$` paragraph from a real fence.
inline const char *mathDelimiterName(MathDelimiter delimiter) {
  switch (delimiter) {
    case MathDelimiter::Dollar: return "dollar";
    case MathDelimiter::DoubleDollar: return "doubleDollar";
    case MathDelimiter::Paren: return "paren";
    case MathDelimiter::Bracket: return "bracket";
    case MathDelimiter::Fence: return "fence";
    case MathDelimiter::MathFence: return "mathFence";
  }
  return "doubleDollar";
}

inline const char *tokenName(TokenClass token) {
  switch (token) {
    case TokenClass::Plain: return "plain";
    case TokenClass::Comment: return "comment";
    case TokenClass::Keyword: return "keyword";
    case TokenClass::Type: return "type";
    case TokenClass::String: return "string";
    case TokenClass::Number: return "number";
    case TokenClass::Constant: return "constant";
    case TokenClass::Regex: return "regex";
    case TokenClass::Function: return "function";
    case TokenClass::Property: return "property";
    case TokenClass::Operator: return "operator";
    case TokenClass::Preprocessor: return "preprocessor";
    case TokenClass::Tag: return "tag";
    case TokenClass::Attribute: return "attribute";
    case TokenClass::Inserted: return "inserted";
    case TokenClass::Deleted: return "deleted";
    case TokenClass::Heading: return "heading";
    case TokenClass::Count: break;
  }
  return "unknown";
}

inline void writeString(std::string_view text) {
  std::putchar('"');
  for (const char c : text) {
    const auto byte = static_cast<unsigned char>(c);
    switch (c) {
      case '"':
        std::fputs("\\\"", stdout);
        break;
      case '\\':
        std::fputs("\\\\", stdout);
        break;
      case '\n':
        std::fputs("\\n", stdout);
        break;
      case '\t':
        std::fputs("\\t", stdout);
        break;
      default:
        if (byte < 0x20) {
          std::printf("\\u%04x", byte);
        } else {
          std::putchar(c);
        }
    }
  }
  std::putchar('"');
}

inline void writeString(const std::u16string &text) {
  writeString(utf16ToUtf8(text));
}

/// Prints `{"error":"<code>"}` for a parse that did not produce a document.
inline void writeError(const char *code) {
  std::printf("{\"error\":");
  writeString(std::string(code));
  std::printf("}\n");
}

/// Prints the document, or the error when the parse failed, and reports which
/// it printed. Identity and sourceRange are left out: they describe where a
/// block came from, and every parse mints new ones. When it printed a document
/// the closing brace is left to the caller, which may have counters to add.
inline bool write(const ParseResult &result) {
  if (result.status != ParseStatus::Ok || !result.document) {
    writeError(errorCode(result.status));
    return false;
  }

  const Document &document = *result.document;
  std::printf("{\"settled\":%u,\"blocks\":[", document.settledBlocks);
  for (size_t b = 0; b < document.blocks.size(); b++) {
    const Block &block = document.blocks[b];
    std::printf("%s{\"kind\":%d,\"headingLevel\":%d,\"tableHeader\":%s,\"table\":%u,\"spacing\":%d,\"language\":",
                b ? "," : "", static_cast<int>(block.kind), block.headingLevel, block.tableHeader ? "true" : "false",
                block.table, static_cast<int>(block.spacingBefore));
    writeString(block.language);
    std::printf(",\"info\":");
    writeString(block.info);
    // The parsed meta, so a test can assert on the header label, the gutter, and
    // the download name all reading one struct rather than re-deriving them.
    std::printf(",\"meta\":{\"title\":");
    writeString(block.meta.title);
    std::printf(",\"startLine\":%u,\"noLineNumbers\":%s,\"marks\":[", block.meta.startLine,
                block.meta.noLineNumbers ? "true" : "false");
    for (size_t m = 0; m < block.meta.marks.size(); m++) {
      std::printf("%s[%u,%u]", m ? "," : "", block.meta.marks[m].first, block.meta.marks[m].last);
    }
    std::printf("]}");
    // The layout direction, so a test can read the rule's answer rather than
    // inferring it from the text. 0 is `Inherit`, which the engine resolves to a
    // resolved direction before the block reaches here, so a block that prints 0
    // is a block the rule left alone.
    std::printf(",\"direction\":%d", static_cast<int>(block.direction));
    std::printf(",\"text\":");
    writeString(block.text);
    std::printf(",\"containers\":[");
    for (size_t c = 0; c < block.containers.size(); c++) {
      const Container &container = block.containers[c];
      // `open` and `tag` are the two fields only a `<details>` and an app's own
      // block tag carry, and they are what a reader's own choice is resolved
      // against, so the model says which of them are present rather than leaving
      // a test to infer it from the kind.
      std::printf("%s{\"kind\":%d,\"id\":%u,\"showsMarker\":%s,\"task\":%d,\"marker\":", c ? "," : "",
                  static_cast<int>(container.kind), container.id, container.showsMarker ? "true" : "false",
                  static_cast<int>(container.task));
      writeString(container.marker);
      std::printf(",\"open\":%s,\"tag\":%d}", container.open ? "true" : "false", container.tag);
    }
    std::printf("],\"runs\":[");
    for (size_t r = 0; r < block.runs.size(); r++) {
      const InlineRun &run = block.runs[r];
      std::printf("%s[%u,%u,%u,%d]", r ? "," : "", run.start, run.end, run.styles, run.link);
    }
    std::printf("],\"tokens\":[");
    for (size_t t = 0; t < block.tokens.size(); t++) {
      const TokenRun &run = block.tokens[t];
      std::printf("%s[%u,%u,\"%s\",", t ? "," : "", run.start, run.end, tokenName(run.token));
      writeString(block.text.substr(run.start, run.end - run.start));
      std::printf("]");
    }
    std::printf("],\"cells\":[");
    for (size_t c = 0; c < block.cells.size(); c++) {
      const TableCell &cell = block.cells[c];
      std::printf("%s[%u,%u,%d]", c ? "," : "", cell.start, cell.end, static_cast<int>(cell.align));
    }
    // The formulas, with their layouts reduced to a shape. A test asserts the
    // offsets, the TeX, the delimiter, and whether a layout arrived; the
    // layout's contents are the engine's business and belong to the engine's
    // own goldens.
    std::printf("],\"math\":[");
    for (size_t m = 0; m < block.math.size(); m++) {
      const MathSpan &span = block.math[m];
      std::printf("%s{\"range\":[%u,%u],\"tex\":", m ? "," : "", span.start, span.end);
      writeString(span.tex);
      std::printf(",\"display\":%s,\"delimiter\":\"%s\",\"layout\":", span.display ? "true" : "false",
                  mathDelimiterName(span.delimiter));
      if (span.layout) {
        std::printf("{\"width\":%.4f,\"height\":%.4f,\"depth\":%.4f,\"items\":%zu}", span.layout->width,
                    span.layout->height, span.layout->depth, span.layout->itemCount());
      } else {
        std::printf("null");
      }
      std::printf(",\"error\":");
      writeString(span.error);
      std::printf("}");
    }
    // The images this block names, so a test can see that an image in prose is
    // recorded but did not become a block.
    std::printf("],\"images\":[");
    for (size_t i = 0; i < block.images.size(); i++) {
      std::printf("%s%u", i ? "," : "", block.images[i]);
    }
    std::printf("]}");
  }
  std::printf("],\"links\":[");
  for (size_t l = 0; l < document.links.size(); l++) {
    const Link &link = document.links[l];
    std::printf("%s{\"url\":", l ? "," : "");
    writeString(link.url);
    std::printf(",\"range\":[%u,%u]}", link.range.start, link.range.end);
  }
  // The images, whether or not any block was promoted to one. An image in
  // prose is still recorded, so a run's alt text can name it and a blocked or
  // failed one has something to fall back from.
  std::printf("],\"images\":[");
  for (size_t i = 0; i < document.images.size(); i++) {
    const Image &image = document.images[i];
    std::printf("%s{\"url\":", i ? "," : "");
    writeString(image.url);
    std::printf(",\"alt\":");
    writeString(image.alt);
    std::printf(",\"title\":");
    writeString(image.title);
    std::printf(",\"loadable\":%s,\"link\":%d,\"range\":[%u,%u]}", image.loadable ? "true" : "false", image.link,
                image.range.start, image.range.end);
  }
  std::printf("]");
  return true;
}

/**
 * The parse options from a driver's argument, if it was given one.
 *
 * `dump_document [parseFlags]`, `dump_session <mode> [parseFlags]`, and the rest
 * all take the same number and mean the same thing, so there is one reader.
 * Absent means off, which is what makes the whole existing corpus byte-identical
 * to what it was before math existed; a case that is about a formula names its
 * flags.
 */
inline ParseOptions parseFlagsFromArg(int argc, char **argv, int at) {
  ParseOptions options;
  options.flags = 0;
  if (argc <= at) {
    return options;
  }
  options.flags = static_cast<uint32_t>(std::strtoul(argv[at], nullptr, 10));
  return options;
}

/// The tag policy from argv[at], as `src/allowedTags.ts` writes it, so a driver
/// exercises the engine's own reader rather than hand-building a policy. Absent
/// is the empty policy, which is what the built-in table on its own means.
inline ParseOptions parseOptionsFromArgs(int argc, char **argv) {
  ParseOptions options = parseFlagsFromArg(argc, argv, 1);
  if (argc <= 2) {
    return options;
  }
  const std::string joined(argv[2] == nullptr ? "" : argv[2]);
  if (joined.empty()) {
    return options;
  }
  std::vector<std::string> flat;
  size_t start = 0;
  while (start <= joined.size()) {
    const size_t comma = joined.find(',', start);
    flat.push_back(joined.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
    if (comma == std::string::npos) {
      break;
    }
    start = comma + 1;
  }
  return ParseOptions(options.flags, tagPolicyFromStrings(flat));
}

/// The registered languages from argv[at], comma separated and unnormalized,
/// exactly as JavaScript sends them, so a driver exercises the engine's own
/// normalization. Absent or empty is no languages.
inline std::vector<std::string> languagesFromArg(int argc, char **argv, int at) {
  std::vector<std::string> languages;
  if (at >= argc || argv[at] == nullptr) {
    return languages;
  }
  const std::string joined(argv[at]);
  size_t start = 0;
  while (start <= joined.size()) {
    const size_t comma = joined.find(',', start);
    const std::string one = joined.substr(start, comma == std::string::npos ? comma : comma - start);
    if (!one.empty()) {
      languages.push_back(one);
    }
    if (comma == std::string::npos) {
      break;
    }
    start = comma + 1;
  }
  return languages;
}

/// Fence slots as the JSON array `tests/engine.ts` reads, in the shape the
/// TurboModule bridges them.
inline void writeFenceSlots(const std::vector<FenceSlot> &slots) {
  std::printf("[");
  for (size_t index = 0; index < slots.size(); index++) {
    const FenceSlot &slot = slots[index];
    std::printf("%s{\"ordinal\":%u,\"blockIndex\":%zu,\"language\":", index ? "," : "", slot.ordinal,
                slot.blockIndex);
    writeString(slot.language);
    std::printf(",\"meta\":");
    writeString(slot.meta);
    std::printf(",\"code\":");
    writeString(slot.code);
    std::printf(",\"incomplete\":%s,\"identity\":%llu}", slot.incomplete ? "true" : "false",
                static_cast<unsigned long long>(slot.identity));
  }
  std::printf("]");
}

/// Closes what write left open and ends the line. A driver that has more to
/// add prints it in between.
inline void writeModelClosed() {
  std::printf("}\n");
}

/// Prints a whole document object, braces included, for nesting inside
/// another one.
inline void writeDocument(const ParseResult &result) {
  if (write(result)) {
    writeModelClosed();
  }
}

} // namespace downstream::testdump
