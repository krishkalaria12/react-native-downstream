#include "MathDisplayList.h"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <string>

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
/// The protocol sends `{r, g, b, a}` in 0–255. The engine was asked for a
/// sentinel as its default, so this is the one place a layout stops carrying a
/// theme: an item whose color is exactly the sentinel draws in whatever color
/// the run it sits in already has, which is how dark mode, a theme and increased
/// contrast all reach a formula without laying it out again.
///
/// A color that is absent, malformed, or carries no recognized component is the
/// sentinel rather than a failure. One item drawing in the run's color is right
/// far more often than one formula showing its source, and `\color{red}` still
/// comes through with its own color.
uint32_t readColor(Reader &reader) {
  if (!reader.peek('{')) {
    return kInheritColor;
  }
  int components[4] = {0, 0, 0, 0};
  int seen = 0;
  const bool ok = reader.object([&](const std::string &key, Reader &inner) {
    double value = 0;
    if (!inner.number(&value)) {
      return false;
    }
    if (key == "r" || key == "g" || key == "b" || key == "a") {
      components[key == "r" ? 0 : key == "g" ? 1 : key == "b" ? 2 : 3] =
          value < 0 ? 0 : (value > 255 ? 255 : static_cast<int>(value));
      seen++;
    }
    // A key this build does not know is still consumed, which is what the
    // protocol requires of every field.
    return true;
  });
  if (!ok || seen == 0) {
    return kInheritColor;
  }
  if (components[0] == 0 && components[1] == 0 && components[2] == 0 && components[3] == 0) {
    return kInheritColor;
  }
  return (static_cast<uint32_t>(components[3]) << 24) | (static_cast<uint32_t>(components[0]) << 16) |
      (static_cast<uint32_t>(components[1]) << 8) | static_cast<uint32_t>(components[2]);
}

PathOp pathOpFromName(std::string_view name) {
  if (name == "Move") return PathOp::Move;
  if (name == "Quad") return PathOp::Quad;
  if (name == "Cubic") return PathOp::Cubic;
  if (name == "Close") return PathOp::Close;
  // Anything else is a line, which is what a protocol that adds a command
  // should degrade to rather than refusing the whole formula over.
  return PathOp::Line;
}

/// One item out of the list.
///
/// An item type this build does not know is skipped whole, which is what makes
/// a RaTeX version that adds an item type a non-event: one item this build
/// cannot draw, rather than a formula that fails.
bool decodeItem(Reader &reader, MathLayout *layout) {
  if (!reader.peek('{')) {
    return reader.fail("A display list item is not an object.");
  }
  // The three shapes are read into one set of fields and only the one that
  // matches the item's type is kept. Their field names do not collide, and one
  // reader is smaller than three that would each have to track the schema.
  MathGlyph glyph;
  MathRule rule;
  MathPath path;
  int kind = 0; // 1 glyph, 2 rule, 3 path
  const bool ok = reader.object(
      [&](const std::string &key, Reader &inner) {
        if (key == "type") {
          std::string name;
          if (!inner.string(&name)) {
            return false;
          }
          if (name == "GlyphPath" || name == "Glyph") {
            kind = 1;
          } else if (name == "Line" || name == "Rect") {
            kind = 2;
          } else if (name == "Path" || name == "Stroke" || name == "Fill") {
            kind = 3;
          }
          return true;
        }
        if (key == "x" || key == "y" || key == "width" || key == "height" || key == "thickness" ||
            key == "scale" || key == "stroke_width") {
          float value = 0;
          if (!inner.coordinate(&value)) {
            return false;
          }
          if (key == "x") {
            glyph.x = path.x = value;
          } else if (key == "y") {
            glyph.y = path.y = value;
          } else if (key == "width" || key == "thickness") {
            rule.width = value;
          } else if (key == "height") {
            rule.height = value;
          } else if (key == "scale") {
            glyph.scale = value > 0 ? value : 1.0f;
          } else {
            path.strokeWidth = value;
          }
          return true;
        }
        if (key == "font") {
          std::string name;
          if (!inner.string(&name)) {
            return false;
          }
          glyph.font = fontFromName(name);
          return true;
        }
        if (key == "char_code" || key == "code_point" || key == "char") {
          double code = 0;
          if (!inner.number(&code)) {
            return false;
          }
          if (code < 0 || code > static_cast<double>(limits::kMaxCodePoint)) {
            return inner.fail("A display list code point is out of range.");
          }
          glyph.codePoint = static_cast<char32_t>(code);
          return true;
        }
        if (key == "color") {
          const uint32_t color = readColor(inner);
          glyph.color = rule.color = path.color = color;
          return true;
        }
        if (key == "dashed" || key == "fill") {
          double flag = 0;
          if (!inner.number(&flag)) {
            return false;
          }
          if (key == "dashed") {
            rule.dashed = flag != 0;
          } else {
            path.fill = flag != 0;
          }
          return true;
        }
        if (key == "ops" || key == "path" || key == "commands") {
          if (!inner.peek('[')) {
            // A path with no commands draws nothing, which is a legal thing for
            // a protocol to send and not worth refusing the formula over.
            return true;
          }
          return inner.array([&](Reader &element) {
            if (path.commands.size() >= limits::kMaxPathCommands) {
              return element.fail("A display list path has too many commands.");
            }
            PathCommand command;
            const bool read = element.object([&](const std::string &field, Reader &values) {
              if (field == "type" || field == "op") {
                std::string name;
                if (!values.string(&name)) {
                  return false;
                }
                command.op = pathOpFromName(name);
                return true;
              }
              float value = 0;
              if (!values.coordinate(&value)) {
                return false;
              }
              if (field == "x") {
                command.x = value;
              } else if (field == "y") {
                command.y = value;
              } else if (field == "x2") {
                command.x2 = value;
              } else if (field == "y2") {
                command.y2 = value;
              }
              // Any other field is consumed and dropped, which is the same
              // tolerance as at the top level.
              return true;
            });
            if (read) {
              path.commands.push_back(command);
            }
            return read;
          });
        }
        // A key this build does not know. Skipping it whole is what the
        // protocol requires.
        inner.skipValue();
        return true;
      });
  if (!ok) {
    return false;
  }
  switch (kind) {
    case 1: layout->glyphs.push_back(glyph); break;
    case 2: layout->rules.push_back(rule); break;
    case 3: layout->paths.push_back(std::move(path)); break;
    default: break;
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
