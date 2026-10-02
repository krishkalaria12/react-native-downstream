#include "MathDisplayList.h"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

namespace downstream::math {

namespace {

/**
 * A reader over one display list.
 *
 * Hand-written rather than a library, so the bounds the protocol needs are
 * enforced in the same pass that reads the value: a `NaN` width, a nesting
 * depth over the limit, and a hundred thousand items are all refused where they
 * appear rather than after a list has been built. It also means a list cannot
 * make this allocate more than the limits allow, which matters because the
 * engine runs on a worker thread inside a synchronous layout budget.
 *
 * `object` and `array` are the only two shapes the schema has, and each hands
 * its caller a key or nothing, leaving the value to be consumed by a callback.
 * That is what keeps the tolerance in one place: a caller that does not know a
 * key calls `skipValue` and moves on, and the reader is back at the next comma.
 */
class Reader {
public:
  using ObjectVisitor = std::function<bool(const std::string &key, Reader &reader)>;
  using ArrayVisitor = std::function<bool(Reader &reader)>;

  explicit Reader(std::string_view text) : text_(text) {}

  void skipSpace() {
    while (offset_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[offset_]))) {
      offset_++;
    }
  }

  bool atEnd() {
    skipSpace();
    return offset_ >= text_.size();
  }

  bool peek(char c) {
    skipSpace();
    return offset_ < text_.size() && text_[offset_] == c;
  }

  bool take(char c) {
    if (!peek(c)) {
      return false;
    }
    offset_++;
    return true;
  }

  /// Records the first failure only, so a caller's message names the cause
  /// rather than whatever happened last.
  bool fail(std::string message) {
    if (error_.empty()) {
      error_ = std::move(message);
    }
    return false;
  }

  const std::string &error() const { return error_; }

  /// Reads a `{…}` object, calling `visit` for each member. The callback must
  /// consume the member's value; returning false ends the read and is the
  /// callback's way of refusing the whole list.
  bool object(ObjectVisitor visit) {
    enter();
    if (!take('{')) {
      leave();
      return fail("A display list object is missing its brace.");
    }
    if (take('}')) {
      leave();
      return true;
    }
    while (true) {
      std::string key;
      if (!string(&key) || !take(':')) {
        leave();
        return error_.empty() ? fail("A display list member is malformed.") : false;
      }
      if (!visit(key, *this)) {
        leave();
        return false;
      }
      if (take(',')) {
        if (take('}')) {
          break;
        }
        continue;
      }
      if (take('}')) {
        leave();
        return true;
      }
      leave();
      return fail("A display list object is missing a comma.");
    }
    leave();
    return true;
  }

  /// Reads a `[…]` array, calling `visit` once per element.
  bool array(ArrayVisitor visit) {
    enter();
    if (!take('[')) {
      leave();
      return fail("A display list array is missing its bracket.");
    }
    if (take(']')) {
      leave();
      return true;
    }
    while (true) {
      if (!visit(*this)) {
        leave();
        return false;
      }
      if (take(',')) {
        if (take(']')) {
          break;
        }
        continue;
      }
      if (take(']')) {
        leave();
        return true;
      }
      leave();
      return fail("A display list array is missing a comma.");
    }
    leave();
    return true;
  }

  /// A number, or false. A non-finite value is refused here rather than at draw
  /// time, because a `NaN` coordinate makes CoreText and Canvas both draw
  /// nothing at all, which reads as a formula that silently vanished.
  bool number(double *out) {
    skipSpace();
    const size_t start = offset_;
    if (offset_ < text_.size() && (text_[offset_] == '-' || text_[offset_] == '+')) {
      offset_++;
    }
    bool digits = false;
    while (offset_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[offset_]))) {
      offset_++;
      digits = true;
    }
    if (offset_ < text_.size() && text_[offset_] == '.') {
      offset_++;
      while (offset_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[offset_]))) {
        offset_++;
        digits = true;
      }
    }
    if (!digits) {
      offset_ = start;
      return fail("A display list value is not a number.");
    }
    if (offset_ < text_.size() && (text_[offset_] == 'e' || text_[offset_] == 'E')) {
      const size_t exponent = offset_;
      offset_++;
      if (offset_ < text_.size() && (text_[offset_] == '-' || text_[offset_] == '+')) {
        offset_++;
      }
      bool exponentDigits = false;
      while (offset_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[offset_]))) {
        offset_++;
        exponentDigits = true;
      }
      if (!exponentDigits) {
        offset_ = exponent;
        return fail("A display list exponent is malformed.");
      }
    }
    const std::string literal(text_.substr(start, offset_ - start));
    *out = std::strtod(literal.c_str(), nullptr);
    return std::isfinite(*out) || fail("A display list value is not finite.");
  }

  bool coordinate(float *out) {
    double value = 0;
    if (!number(&value)) {
      return false;
    }
    *out = static_cast<float>(value);
    return std::isfinite(*out) || fail("A display list value does not fit a coordinate.");
  }

  /// A boolean. RaTeX writes `true` and `false`; a number is read as its
  /// truthiness and `null` as false, which is the tolerant reading the protocol
  /// asks for of an optional field.
  bool flag(bool *out) {
    skipSpace();
    const std::string_view rest = text_.substr(offset_);
    if (rest.substr(0, 4) == "true") {
      offset_ += 4;
      *out = true;
      return true;
    }
    if (rest.substr(0, 5) == "false") {
      offset_ += 5;
      *out = false;
      return true;
    }
    if (rest.substr(0, 4) == "null") {
      offset_ += 4;
      *out = false;
      return true;
    }
    double value = 0;
    if (!number(&value)) {
      return false;
    }
    *out = value != 0;
    return true;
  }

  /// A string, unescaped. An escape this build does not know is taken as the
  /// character itself, which is the tolerant reading the protocol asks for; a
  /// truncated `\u` is the one thing it does not allow.
  bool string(std::string *out) {
    if (!take('"')) {
      return fail("A display list string is not quoted.");
    }
    out->clear();
    while (offset_ < text_.size()) {
      const char c = text_[offset_++];
      if (c == '"') {
        return true;
      }
      if (c != '\\') {
        out->push_back(c);
        continue;
      }
      if (offset_ >= text_.size()) {
        return fail("A display list string ends in a backslash.");
      }
      const char escape = text_[offset_++];
      switch (escape) {
        case '"': out->push_back('"'); break;
        case '\\': out->push_back('\\'); break;
        case '/': out->push_back('/'); break;
        case 'b': out->push_back('\b'); break;
        case 'f': out->push_back('\f'); break;
        case 'n': out->push_back('\n'); break;
        case 'r': out->push_back('\r'); break;
        case 't': out->push_back('\t'); break;
        case 'u': {
          uint32_t code = 0;
          if (!hex4(&code)) {
            return false;
          }
          // A surrogate pair, because a code point above the basic plane is two
          // escapes and a lone surrogate is not a code point at all.
          if (code >= 0xd800 && code <= 0xdbff && offset_ + 1 < text_.size() && text_[offset_] == '\\' &&
              text_[offset_ + 1] == 'u') {
            const size_t saved = offset_;
            offset_ += 2;
            uint32_t low = 0;
            if (hex4(&low) && low >= 0xdc00 && low <= 0xdfff) {
              code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
            } else {
              offset_ = saved;
              code = 0xfffd;
            }
          }
          appendCodePoint(*out, code);
          break;
        }
        default: out->push_back(escape); break;
      }
    }
    return fail("A display list string is not closed.");
  }

  /// Steps over one value of any type, so an unknown field costs nothing.
  void skipValue() {
    if (atEnd() || tooDeep()) {
      return;
    }
    const char c = text_[offset_];
    if (c == '{') {
      object([](const std::string &, Reader &reader) {
        reader.skipValue();
        return true;
      });
      return;
    }
    if (c == '[') {
      array([](Reader &reader) {
        reader.skipValue();
        return true;
      });
      return;
    }
    if (c == '"') {
      std::string ignored;
      string(&ignored);
      return;
    }
    if (c == 't' || c == 'f' || c == 'n') {
      while (offset_ < text_.size() && std::isalpha(static_cast<unsigned char>(text_[offset_]))) {
        offset_++;
      }
      return;
    }
    double ignored = 0;
    number(&ignored);
  }

  void enter() { depth_++; }
  void leave() { depth_--; }
  bool tooDeep() const { return depth_ > limits::kMaxDepth; }

private:
  bool hex4(uint32_t *out) {
    if (offset_ + 4 > text_.size()) {
      return fail("A display list escape is truncated.");
    }
    uint32_t value = 0;
    for (int i = 0; i < 4; i++) {
      const char c = text_[offset_++];
      value <<= 4;
      if (c >= '0' && c <= '9') {
        value |= static_cast<uint32_t>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        value |= static_cast<uint32_t>(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
        value |= static_cast<uint32_t>(c - 'A' + 10);
      } else {
        return fail("A display list escape is not hexadecimal.");
      }
    }
    *out = value;
    return true;
  }

  static void appendCodePoint(std::string &out, uint32_t code) {
    if (code > limits::kMaxCodePoint || (code >= 0xd800 && code <= 0xdfff)) {
      code = 0xfffd;
    }
    if (code < 0x80) {
      out.push_back(static_cast<char>(code));
    } else if (code < 0x800) {
      out.push_back(static_cast<char>(0xc0 | (code >> 6)));
      out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
    } else if (code < 0x10000) {
      out.push_back(static_cast<char>(0xe0 | (code >> 12)));
      out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
      out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
    } else {
      out.push_back(static_cast<char>(0xf0 | (code >> 18)));
      out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3f)));
      out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
      out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
    }
  }

  std::string_view text_;
  size_t offset_{0};
  size_t depth_{0};
  std::string error_;
};

/// A color, as an ARGB word, or `kInheritColor`.
///
/// The protocol sends `{r, g, b, a}` as floats from 0 to 1. The engine asked for
/// a sentinel of all zeros as its default, so this is the one place a layout
/// stops carrying a theme: an item whose color is exactly the sentinel draws in
/// whatever color the run it sits in already has, which is how dark mode, a
/// theme and increased contrast all reach a formula without laying it out again.
///
/// A color that is absent, malformed, or carries no recognized component is the
/// sentinel rather than a failure. One item drawing in the run's color is right
/// far more often than one formula showing its source, and `\color{red}` still
/// comes through with its own color.
uint32_t readColor(Reader &reader) {
  if (!reader.peek('{')) {
    reader.skipValue();
    return kInheritColor;
  }
  double components[4] = {0, 0, 0, 0};
  int seen = 0;
  const bool ok = reader.object([&](const std::string &key, Reader &inner) {
    double value = 0;
    if (!inner.number(&value)) {
      return false;
    }
    if (key == "r" || key == "g" || key == "b" || key == "a") {
      components[key == "r" ? 0 : key == "g" ? 1 : key == "b" ? 2 : 3] = value < 0 ? 0 : (value > 1 ? 1 : value);
      seen++;
    }
    // A key this build does not know is still consumed, which is what the
    // protocol requires of every field.
    return true;
  });
  if (!ok || seen == 0) {
    return kInheritColor;
  }
  uint32_t bytes[4];
  for (int i = 0; i < 4; i++) {
    bytes[i] = static_cast<uint32_t>(std::lround(components[i] * 255.0));
  }
  if (bytes[0] == 0 && bytes[1] == 0 && bytes[2] == 0 && bytes[3] == 0) {
    return kInheritColor;
  }
  return (bytes[3] << 24) | (bytes[0] << 16) | (bytes[1] << 8) | bytes[2];
}

/// A path command's type, or false for one this build does not know.
bool pathOpFromName(std::string_view name, PathOp *op) {
  if (name == "MoveTo") *op = PathOp::Move;
  else if (name == "LineTo") *op = PathOp::Line;
  else if (name == "QuadTo") *op = PathOp::Quad;
  else if (name == "CubicTo") *op = PathOp::Cubic;
  else if (name == "Close") *op = PathOp::Close;
  else return false;
  return true;
}

/// One item out of the list.
///
/// An item type this build does not know is skipped whole, and so is a path
/// command, which is what makes a RaTeX version that adds either a non-event:
/// one shape this build cannot draw, rather than a formula that fails.
///
/// The fields are read first and the shape is built after, because the protocol
/// does not order fields and the same name means different things per type: a
/// `Line`'s `y` is its centre and its `width` a length, a `Rect`'s `y` is
/// its top.
bool decodeItem(Reader &reader, MathLayout *layout) {
  if (!reader.peek('{')) {
    return reader.fail("A display list item is not an object.");
  }
  std::string type;
  float x = 0, y = 0, width = 0, height = 0, thickness = 0, scale = 1, strokeWidth = 0;
  bool dashed = false;
  bool fill = false;
  MathFont font = MathFont::MainRegular;
  char32_t codePoint = 0;
  uint32_t color = kInheritColor;
  std::vector<PathCommand> commands;
  const bool ok = reader.object([&](const std::string &key, Reader &inner) {
    if (key == "type") {
      return inner.string(&type);
    }
    if (key == "x") return inner.coordinate(&x);
    if (key == "y") return inner.coordinate(&y);
    if (key == "width") return inner.coordinate(&width);
    if (key == "height") return inner.coordinate(&height);
    if (key == "thickness") return inner.coordinate(&thickness);
    if (key == "stroke_width") return inner.coordinate(&strokeWidth);
    if (key == "scale") {
      if (!inner.coordinate(&scale)) {
        return false;
      }
      if (!(scale > 0)) {
        scale = 1;
      }
      return true;
    }
    if (key == "font") {
      std::string name;
      if (!inner.string(&name)) {
        return false;
      }
      font = fontFromName(name);
      return true;
    }
    if (key == "char_code") {
      double code = 0;
      if (!inner.number(&code)) {
        return false;
      }
      if (code < 0 || code > static_cast<double>(limits::kMaxCodePoint)) {
        return inner.fail("A display list code point is out of range.");
      }
      codePoint = static_cast<char32_t>(code);
      return true;
    }
    if (key == "color") {
      color = readColor(inner);
      return true;
    }
    if (key == "dashed") return inner.flag(&dashed);
    if (key == "fill") return inner.flag(&fill);
    if (key == "commands") {
      if (!inner.peek('[')) {
        // A path with no commands draws nothing, which is a legal thing for a
        // protocol to send and not worth refusing the formula over.
        inner.skipValue();
        return true;
      }
      return inner.array([&](Reader &element) {
        if (commands.size() >= limits::kMaxPathCommands) {
          return element.fail("A display list path has too many commands.");
        }
        PathCommand command;
        bool known = false;
        const bool read = element.object([&](const std::string &field, Reader &values) {
          if (field == "type") {
            std::string name;
            if (!values.string(&name)) {
              return false;
            }
            known = pathOpFromName(name, &command.op);
            return true;
          }
          float value = 0;
          if (field == "x" || field == "y" || field == "x1" || field == "y1" || field == "x2" || field == "y2") {
            if (!values.coordinate(&value)) {
              return false;
            }
            (field == "x"    ? command.x
             : field == "y"  ? command.y
             : field == "x1" ? command.x1
             : field == "y1" ? command.y1
             : field == "x2" ? command.x2
                             : command.y2) = value;
            return true;
          }
          // Any other field is consumed and dropped, which is the same tolerance
          // as at the top level.
          values.skipValue();
          return true;
        });
        if (read && known) {
          commands.push_back(command);
        }
        return read;
      });
    }
    // A key this build does not know. Skipping it whole is what the protocol
    // requires.
    inner.skipValue();
    return true;
  });
  if (!ok) {
    return false;
  }
  if (type == "GlyphPath") {
    MathGlyph glyph;
    glyph.x = x;
    glyph.y = y;
    glyph.scale = scale;
    glyph.font = font;
    glyph.codePoint = codePoint;
    glyph.color = color;
    layout->glyphs.push_back(glyph);
  } else if (type == "Line") {
    // The centre line and a thickness, as a box. RaTeX's own renderers never draw
    // a rule thinner than half a point; that floor is the platform's, at draw
    // time, because it is in points and this is in em.
    MathRule rule;
    rule.x = x;
    rule.y = y - thickness / 2;
    rule.width = width;
    rule.height = thickness;
    rule.color = color;
    rule.dashed = dashed;
    layout->rules.push_back(rule);
  } else if (type == "Rect") {
    MathRule rule;
    rule.x = x;
    rule.y = y;
    rule.width = width;
    rule.height = height;
    rule.color = color;
    layout->rules.push_back(rule);
  } else if (type == "Path") {
    MathPath path;
    path.x = x;
    path.y = y;
    path.commands = std::move(commands);
    path.fill = fill;
    path.strokeWidth = strokeWidth;
    path.color = color;
    layout->paths.push_back(std::move(path));
  }
  return true;
}

} // namespace

MathFont fontFromName(std::string_view name) {
  for (uint8_t i = 0; i <= static_cast<uint8_t>(MathFont::System); i++) {
    if (mathFontName(static_cast<MathFont>(i)) == name) {
      return static_cast<MathFont>(i);
    }
  }
  return MathFont::System;
}

std::string_view fileNameForFont(MathFont font) { return mathFontFileName(font); }

std::string_view postScriptNameForFont(MathFont font) { return mathFontPostScriptName(font); }

bool decodeDisplayList(std::string_view json, MathLayout *out, std::string *error) {
  const auto refuse = [&](const std::string &message) {
    if (error != nullptr && error->empty()) {
      *error = message;
    }
    return false;
  };
  if (out == nullptr) {
    return refuse("A display list was decoded into nothing.");
  }
  MathLayout layout;
  Reader reader(json);
  size_t items = 0;
  if (!reader.object(
          [&](const std::string &key, Reader &inner) {
            if (key == "width" || key == "height" || key == "depth") {
              float value = 0;
              if (!inner.coordinate(&value)) {
                return false;
              }
              if (key == "width") {
                layout.width = value;
              } else if (key == "height") {
                layout.height = value;
              } else {
                layout.depth = value;
              }
              return true;
            }
            if (key == "items") {
              if (!inner.peek('[')) {
                return inner.fail("A display list has no item array.");
              }
              return inner.array([&](Reader &element) {
                if (element.tooDeep()) {
                  return element.fail("A display list nests too deeply.");
                }
                if (++items > limits::kMaxItems) {
                  return element.fail("A display list has too many items.");
                }
                return decodeItem(element, &layout);
              });
            }
            // `version` and every field this build does not know. The protocol
            // says to ignore them, and that is what makes a RaTeX version bump
            // a change to this file rather than a crash.
            inner.skipValue();
            return true;
          })) {
    return refuse(reader.error().empty() ? "A display list is malformed." : reader.error());
  }

  // A width or a height outside the bounds is a formula built to exhaust
  // something rather than one anybody will read, and refusing it here is what
  // stops `\rule{1e6em}{1e6em}` from becoming a 2000-em-wide box.
  if (!(layout.width >= 0) || !(layout.width <= limits::kMaxWidthEm) || !(layout.height >= 0) ||
      !(layout.depth >= 0)) {
    return refuse("A display list is not a plausible formula.");
  }
  *out = std::move(layout);
  return true;
}

} // namespace downstream::math
