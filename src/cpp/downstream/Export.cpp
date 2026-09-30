#include "Export.h"

#include "ExportNames.h"
#include "Unicode.h"

#include <algorithm>
#include <string>
#include <vector>

namespace downstream {
namespace {

// Streamdown's table exports and this one's agree on these. The two CSV rules
// below are its `tableDataToCSV` and `tableDataToTSV` verbatim, so a value
// that survives a round trip through the web component survives through this.

// ------------------------------------------------------------------- cells

/// Visible text of one table cell, with the model's tabs and newlines left as
/// they are: a cell holding a line break is a cell holding a line break, and
/// each format escapes it its own way.
std::u16string_view cellText(const Block &block, const TableCell &cell) {
  const uint32_t start = std::min<uint32_t>(cell.start, block.text.size());
  const uint32_t end = std::min<uint32_t>(std::max(cell.start, cell.end), block.text.size());
  return std::u16string_view(block.text).substr(start, end - start);
}

/// Streamdown's `escapeCSV`: a value is quoted only when it holds a character
/// that would otherwise end the field, and an inner quote is doubled.
void appendCsvValue(std::u16string &out, std::u16string_view value, char16_t separator) {
  bool quote = false;
  for (const char16_t unit : value) {
    if (unit == separator || unit == u'"' || unit == u'\n' || unit == u'\r') {
      quote = true;
      break;
    }
  }
  if (!quote) {
    out.append(value);
    return;
  }
  out.push_back(u'"');
  for (const char16_t unit : value) {
    if (unit == u'"') {
      out.push_back(u'"');
    }
    out.push_back(unit);
  }
  out.push_back(u'"');
}

/// Streamdown's `escapeTSV`: no quoting, because a tab inside a TSV cell would
/// read as a new column, so a tab, a newline, and a carriage return become the
/// two-character escapes a spreadsheet shows as text.
void appendTsvValue(std::u16string &out, std::u16string_view value) {
  for (const char16_t unit : value) {
    switch (unit) {
      case u'\t':
        out.append(u"\\t");
        break;
      case u'\n':
        out.append(u"\\n");
        break;
      case u'\r':
        out.append(u"\\r");
        break;
      default:
        out.push_back(unit);
        break;
    }
  }
}

void exportDelimited(const Document &document, size_t from, size_t to, std::u16string &out, char16_t separator,
                     bool tabSeparated) {
  for (size_t index = from; index < to; index++) {
    if (index > from) {
      out.push_back(u'\n');
    }
    const Block &block = document.blocks[index];
    for (size_t column = 0; column < block.cells.size(); column++) {
      if (column > 0) {
        out.push_back(separator);
      }
      const std::u16string_view value = cellText(block, block.cells[column]);
      if (tabSeparated) {
        appendTsvValue(out, value);
      } else {
        appendCsvValue(out, value, separator);
      }
    }
  }
}

// ---------------------------------------------------------------- markdown

/// Characters that would otherwise be read as Markdown rather than as the text
/// the reader sees. Backslash leads because it is the escape character itself.
constexpr std::u16string_view kMarkdownEscapes = u"\\|*_~`[]<>&";

void appendMarkdownPlain(std::u16string &out, std::u16string_view text) {
  for (const char16_t unit : text) {
    if (kMarkdownEscapes.find(unit) != std::u16string_view::npos) {
      out.push_back(u'\\');
    }
    out.push_back(unit);
  }
}

/// The longest run of backticks in `text`, which is how a code span's fence is
/// chosen: it has to be longer than anything inside it, or the inner run would
/// close it early.
size_t longestBacktickRun(std::u16string_view text) {
  size_t longest = 0;
  size_t run = 0;
  for (const char16_t unit : text) {
    run = unit == u'`' ? run + 1 : 0;
    longest = std::max(longest, run);
  }
  return longest;
}

/// A code span with a fence one backtick longer than the longest run inside,
/// and one space of padding on each side when the content itself starts or ends
/// with a backtick. The spaces go inside the fence: that is where CommonMark
/// looks for them, and it is what keeps the closing run from merging with the
/// content's own trailing run and becoming unclosable.
void appendMarkdownCode(std::u16string &out, std::u16string_view text) {
  const std::u16string fence(longestBacktickRun(text) + 1, u'`');
  const bool pad = !text.empty() && (text.front() == u'`' || text.back() == u'`');
  out.append(fence);
  if (pad) {
    out.push_back(u' ');
  }
  out.append(text);
  if (pad) {
    out.push_back(u' ');
  }
  out.append(fence);
}

/// The runs of a cell that overlap it, so the visible text can be written back
/// with its styles. Runs are sorted and never overlap, and a cell's own range
/// clamps them.
struct CellRun {
  uint32_t start;
  uint32_t end;
  uint32_t styles;
  int32_t link;
};

std::vector<CellRun> cellRuns(const Block &block, const TableCell &cell) {
  std::vector<CellRun> runs;
  for (const InlineRun &run : block.runs) {
    const uint32_t start = std::max(run.start, cell.start);
    const uint32_t end = std::min(run.end, cell.end);
    if (start < end) {
      runs.push_back({start, end, run.styles, run.link});
    }
  }
  return runs;
}

/// Splits a cell's visible text into unstyled gaps and styled runs, in order.
struct Piece {
  uint32_t start;
  uint32_t end;
  uint32_t styles;
  int32_t link;
};

std::vector<Piece> pieces(const Block &block, const TableCell &cell) {
  std::vector<Piece> out;
  const uint32_t start = std::min<uint32_t>(cell.start, block.text.size());
  const uint32_t end = std::min<uint32_t>(std::max(cell.start, cell.end), block.text.size());
  uint32_t cursor = start;
  for (const CellRun &run : cellRuns(block, cell)) {
    if (run.start > cursor) {
      out.push_back({cursor, run.start, 0, -1});
    }
    out.push_back({run.start, run.end, run.styles, run.link});
    cursor = run.end;
  }
  if (cursor < end) {
    out.push_back({cursor, end, 0, -1});
  }
  return out;
}

/// A destination is written back only when the link policy already accepted it,
/// so an export never produces a target the reader could not have tapped.
const Link *acceptedLink(const Document &document, int32_t index) {
  if (index < 0 || static_cast<size_t>(index) >= document.links.size()) {
    return nullptr;
  }
  return &document.links[static_cast<size_t>(index)];
}

/**
 * `headerBold` is true for a table's header row, whose whole text carries
 * Strong in the model because a GFM header is always bold. Writing that back
 * out would put `**` on every header cell of every export, and the destination
 * would bold it again anyway, so the implicit weight is dropped and only the
 * styles the writer actually wrote survive.
 */
void appendMarkdownCell(std::u16string &out, const Document &document, const Block &block, const TableCell &cell,
                        bool headerBold) {
  for (const Piece &piece : pieces(block, cell)) {
    const std::u16string_view text(block.text.data() + piece.start, piece.end - piece.start);
    const uint32_t styles = headerBold ? piece.styles & ~static_cast<uint32_t>(InlineStyle::Strong) : piece.styles;
    const bool strike = (styles & InlineStyle::Strikethrough) != 0;
    const bool strong = (styles & InlineStyle::Strong) != 0;
    const bool emphasis = (styles & InlineStyle::Emphasis) != 0;
    const bool code = (styles & InlineStyle::Code) != 0;
    const Link *link = acceptedLink(document, piece.link);
    // Outermost first and code innermost, so re-parsing nests the emphasis
    // inside the link the way the original span nest.
    if (link != nullptr) {
      out.push_back(u'[');
    }
    if (strike) {
      out.append(u"~~");
    }
    if (strong) {
      out.append(u"**");
    }
    if (emphasis) {
      out.push_back(u'*');
    }
    // Image alt text is plain text today, and a footnote reference is written
    // as the number the reader sees; neither carries its own markup.
    if (code) {
      appendMarkdownCode(out, text);
    } else {
      appendMarkdownPlain(out, text);
    }
    if (emphasis) {
      out.push_back(u'*');
    }
    if (strong) {
      out.append(u"**");
    }
    if (strike) {
      out.append(u"~~");
    }
    if (link != nullptr) {
      out.append(u"](");
      appendUtf8(out, link->url);
      out.push_back(u')');
    }
  }
}

/// `:--`, `:-:`, `--:`, or `---`. GFM reads the separator row as the column
/// alignment, so this is the one place alignment survives an export.
std::u16string_view separatorFor(CellAlign align) {
  switch (align) {
    case CellAlign::Left:
      return u":--";
    case CellAlign::Center:
      return u":-:";
    case CellAlign::Right:
      return u"--:";
    case CellAlign::Default:
      break;
  }
  return u"---";
}

void exportMarkdown(const Document &document, size_t from, size_t to, std::u16string &out) {
  if (to <= from) {
    return;
  }
  const Block &header = document.blocks[from];
  const size_t columns = header.cells.size();
  if (columns == 0) {
    return;
  }
  for (size_t index = from; index < to; index++) {
    const Block &block = document.blocks[index];
    const bool headerBold = block.tableHeader;
    out.append(u"| ");
    for (size_t column = 0; column < columns; column++) {
      if (column > 0) {
        out.append(u" | ");
      }
      if (column < block.cells.size()) {
        appendMarkdownCell(out, document, block, block.cells[column], headerBold);
      }
    }
    out.append(u" |\n");
    // The separator row goes under the first row only, and carries that row's
    // alignment for every column.
    if (index == from) {
      out.append(u"| ");
      for (size_t column = 0; column < columns; column++) {
        if (column > 0) {
          out.append(u" | ");
        }
        out.append(separatorFor(column < header.cells.size() ? header.cells[column].align
                                                            : CellAlign::Default));
      }
      out.append(u" |\n");
    }
  }
  // Every row ended in a newline, and the export does not.
  if (!out.empty()) {
    out.pop_back();
  }
}

// -------------------------------------------------------------------- html

void appendHtmlEscaped(std::u16string &out, std::u16string_view text) {
  for (const char16_t unit : text) {
    switch (unit) {
      case u'&':
        out.append(u"&amp;");
        break;
      case u'<':
        out.append(u"&lt;");
        break;
      case u'>':
        out.append(u"&gt;");
        break;
      case u'"':
        out.append(u"&quot;");
        break;
      default:
        out.push_back(unit);
        break;
    }
  }
}

void appendHtmlAttribute(std::u16string &out, std::string_view value) {
  appendHtmlEscaped(out, utf8ToUtf16(value));
}

std::u16string_view alignAttribute(CellAlign align) {
  switch (align) {
    case CellAlign::Left:
      return u" style=\"text-align:left\"";
    case CellAlign::Center:
      return u" style=\"text-align:center\"";
    case CellAlign::Right:
      return u" style=\"text-align:right\"";
    case CellAlign::Default:
      break;
  }
  return {};
}

void appendHtmlCell(std::u16string &out, const Document &document, const Block &block, const TableCell &cell,
                    bool header) {
  out.append(header ? u"<th" : u"<td");
  out.append(alignAttribute(cell.align));
  out.append(u">");
  for (const Piece &piece : pieces(block, cell)) {
    const std::u16string_view text(block.text.data() + piece.start, piece.end - piece.start);
    // A GFM header is bold because it is a header, which a `th` is too, so the
    // implicit weight is dropped rather than nested inside a second `strong`.
    const uint32_t styles = header ? piece.styles & ~static_cast<uint32_t>(InlineStyle::Strong) : piece.styles;
    const bool strike = (styles & InlineStyle::Strikethrough) != 0;
    const bool strong = (styles & InlineStyle::Strong) != 0;
    const bool emphasis = (styles & InlineStyle::Emphasis) != 0;
    const bool code = (styles & InlineStyle::Code) != 0;
    const Link *link = acceptedLink(document, piece.link);
    if (link != nullptr) {
      out.append(u"<a href=\"");
      appendHtmlAttribute(out, link->url);
      out.append(u"\">");
    }
    if (strike) {
      out.append(u"<del>");
    }
    if (strong) {
      out.append(u"<strong>");
    }
    if (emphasis) {
      out.append(u"<em>");
    }
    if (code) {
      out.append(u"<code>");
    }
    appendHtmlEscaped(out, text);
    if (code) {
      out.append(u"</code>");
    }
    if (emphasis) {
      out.append(u"</em>");
    }
    if (strong) {
      out.append(u"</strong>");
    }
    if (strike) {
      out.append(u"</del>");
    }
    if (link != nullptr) {
      out.append(u"</a>");
    }
  }
  out.append(header ? u"</th>" : u"</td>");
}

void exportHtml(const Document &document, size_t from, size_t to, std::u16string &out) {
  if (to <= from) {
    return;
  }
  const bool hasHeader = document.blocks[from].tableHeader;
  out.append(u"<table>");
  size_t body = from;
  if (hasHeader) {
    out.append(u"<thead><tr>");
    const Block &block = document.blocks[from];
    for (const TableCell &cell : block.cells) {
      appendHtmlCell(out, document, block, cell, true);
    }
    out.append(u"</tr></thead>");
    body = from + 1;
  }
  out.append(u"<tbody>");
  for (size_t index = body; index < to; index++) {
    const Block &block = document.blocks[index];
    out.append(u"<tr>");
    for (const TableCell &cell : block.cells) {
      appendHtmlCell(out, document, block, cell, false);
    }
    out.append(u"</tr>");
  }
  out.append(u"</tbody></table>");
}

// -------------------------------------------------------------- file names

/// Reserved on one platform or another, plus the separator, so a name never
/// becomes a path or a shell word on the way to a file.
constexpr std::string_view kReservedInNames = "/\\:*?\"<>|";

/// The last path component, so a fence's `src/app.ts` names a file rather than
/// a directory that does not exist.
std::string_view lastComponent(std::string_view name) {
  const size_t cut = name.find_last_of("/\\");
  return cut == std::string_view::npos ? name : name.substr(cut + 1);
}

/// Long enough for any real name and short enough for a save dialog.
constexpr size_t kMaxNameBytes = 96;

std::string sanitizeBase(std::string_view base) {
  std::string out;
  out.reserve(std::min(base.size(), kMaxNameBytes));
  for (const char unit : base) {
    const auto byte = static_cast<unsigned char>(unit);
    if (byte < 0x20 || unit == 0x7f || kReservedInNames.find(unit) != std::string_view::npos) {
      continue;
    }
    out.push_back(unit);
  }
  if (out.size() > kMaxNameBytes) {
    out.resize(kMaxNameBytes);
  }
  // A leading dot would hide the file, and a name of nothing but dots is not a
  // name at all.
  size_t start = 0;
  while (start < out.size() && out[start] == '.') {
    start++;
  }
  out.erase(0, start);
  return out;
}

/// The name inside `title="…"`, or the part of the info string after the
/// language and its colon. Streamdown takes the extension from the language
/// alone; the fence's own name is the improvement, because a writer who named
/// the block meant it.
std::string_view fenceName(std::string_view info) {
  const size_t title = info.find("title=");
  if (title == std::string_view::npos) {
    return {};
  }
  size_t start = title + 6;
  if (start < info.size() && (info[start] == '"' || info[start] == '\'')) {
    const char16_t quote = static_cast<char16_t>(info[start]);
    start++;
    const size_t end = info.find(static_cast<char>(quote), start);
    return end == std::string_view::npos ? std::string_view{} : info.substr(start, end - start);
  }
  size_t end = start;
  while (end < info.size() && info[end] != ' ' && info[end] != '\t') {
    end++;
  }
  return info.substr(start, end - start);
}

/// A fence may write its file name as `ts:src/app.ts`. Only the part after the
/// first colon is a name, and only when the language before it is the one the
/// block already has.
std::string_view pathName(std::string_view info) {
  const size_t colon = info.find(':');
  if (colon == std::string_view::npos) {
    return {};
  }
  std::string_view rest = info.substr(colon + 1);
  while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\t')) {
    rest.remove_prefix(1);
  }
  return rest;
}

bool hasExtension(std::string_view name) {
  const size_t dot = name.rfind('.');
  if (dot == std::string_view::npos || dot == 0) {
    return false;
  }
  return name.find('/', dot) == std::string_view::npos;
}

} // namespace

std::u16string exportCodeText(const Document &document, size_t index) {
  if (index >= document.blocks.size()) {
    return {};
  }
  // Block::text is already the code with the container prefixes removed and the
  // fence's final newline dropped, and a NUL already a replacement character, so
  // it is copied as it stands.
  return document.blocks[index].text;
}

std::u16string exportBlocks(const Document &document, size_t from, size_t to, ExportFormat format,
                            char16_t csvSeparator) {
  if (format == ExportFormat::Text) {
    return to <= from ? std::u16string{} : exportCodeText(document, from);
  }
  const size_t limit = std::min(to, document.blocks.size());
  from = std::min(from, limit);
  std::u16string out;
  switch (format) {
    case ExportFormat::Csv:
      exportDelimited(document, from, limit, out, csvSeparator, false);
      break;
    case ExportFormat::Tsv:
      exportDelimited(document, from, limit, out, u'\t', true);
      break;
    case ExportFormat::Markdown:
      exportMarkdown(document, from, limit, out);
      break;
    case ExportFormat::Html:
      exportHtml(document, from, limit, out);
      break;
    case ExportFormat::Text:
      break;
  }
  return out;
}

std::string downloadName(std::string_view info, std::string_view language, const std::string &configuredBase,
                         const std::string &fallback) {
  std::string_view fence = fenceName(info);
  if (fence.empty()) {
    fence = pathName(info);
  }
  std::string base = sanitizeBase(lastComponent(fence));
  bool keepExtension = false;
  if (base.empty()) {
    base = sanitizeBase(lastComponent(configuredBase));
    if (base.empty()) {
      base = sanitizeBase(lastComponent(fallback));
    }
  } else {
    // A name the writer chose for this block already says what it is.
    keepExtension = hasExtension(base);
  }
  if (base.empty()) {
    base = "file";
  }
  if (keepExtension) {
    return base;
  }
  std::string_view extension = extensionForLanguage(language);
  if (extension.empty()) {
    extension = "txt";
  }
  base.push_back('.');
  base.append(extension);
  return base;
}

std::string tableDownloadName(const std::string &configuredBase, std::string_view extension) {
  std::string base = sanitizeBase(lastComponent(configuredBase));
  if (base.empty()) {
    base = "table";
  }
  base.push_back('.');
  base.append(extension.empty() ? std::string_view("txt") : extension);
  return base;
}

} // namespace downstream
