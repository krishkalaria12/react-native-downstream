#include "MathSpeech.h"

#include <array>
#include <utility>
#include <cctype>
#include <string>

namespace downstream::math {

namespace {

/// A command's spoken name.
///
/// Greek letters, the operators and relations, the delimiters, and the named
/// functions. Lower-case names match TeX's own, so a formula read aloud sounds
/// the way it is written; where a screen reader would say "backslash alpha",
/// this says "alpha", which is what a reader means.
constexpr auto kNames = std::to_array<std::pair<std::string_view, std::string_view>>({
    // Lower-case Greek.
    {"alpha", "alpha"}, {"beta", "beta"}, {"gamma", "gamma"}, {"delta", "delta"},
    {"epsilon", "epsilon"}, {"varepsilon", "epsilon"}, {"zeta", "zeta"}, {"eta", "eta"},
    {"theta", "theta"}, {"vartheta", "theta"}, {"iota", "iota"}, {"kappa", "kappa"},
    {"lambda", "lambda"}, {"mu", "mu"}, {"nu", "nu"}, {"xi", "xi"},
    {"pi", "pi"}, {"varpi", "pi"}, {"rho", "rho"}, {"varrho", "rho"},
    {"sigma", "sigma"}, {"varsigma", "sigma"}, {"tau", "tau"}, {"upsilon", "upsilon"},
    {"phi", "phi"}, {"varphi", "phi"}, {"chi", "chi"}, {"psi", "psi"},
    {"omega", "omega"},
    // Upper-case Greek.
    {"Gamma", "capital gamma"}, {"Delta", "capital delta"}, {"Theta", "capital theta"},
    {"Lambda", "capital lambda"}, {"Xi", "capital xi"}, {"Pi", "capital pi"},
    {"Sigma", "capital sigma"}, {"Upsilon", "capital upsilon"}, {"Phi", "capital phi"},
    {"Psi", "capital psi"}, {"Omega", "capital omega"},
    // Relations.
    {"leq", "is less than or equal to"}, {"le", "is less than or equal to"},
    {"geq", "is greater than or equal to"}, {"ge", "is greater than or equal to"},
    {"neq", "is not equal to"}, {"ne", "is not equal to"},
    {"approx", "is approximately"}, {"equiv", "is equivalent to"},
    {"sim", "is similar to"}, {"simeq", "is asymptotically equal to"},
    {"cong", "is congruent to"}, {"propto", "is proportional to"},
    {"ll", "is much less than"}, {"gg", "is much greater than"},
    {"subset", "is a subset of"}, {"supset", "is a superset of"},
    {"subseteq", "is a subset of or equal to"}, {"supseteq", "is a superset of or equal to"},
    {"in", "is in"}, {"ni", "is an element of"},
    {"perp", "is perpendicular to"}, {"parallel", "is parallel to"},
    // Operators.
    {"times", "times"}, {"div", "divided by"}, {"pm", "plus or minus"},
    {"mp", "minus or plus"}, {"cdot", "times"}, {"ast", "times"},
    {"star", "star"}, {"circ", "ring"}, {"bullet", "bullet"},
    {"oplus", "circled plus"}, {"ominus", "circled minus"}, {"otimes", "circled times"},
    {"cap", "intersection"}, {"cup", "union"},
    {"wedge", "logical and"}, {"lor", "logical or"}, {"neg", "not"},
    {"lnot", "not"}, {"forall", "for all"}, {"exists", "there exists"},
    {"land", "logical and"}, {"emptyset", "the empty set"},
    // Named functions and structures.
    {"frac", ""}, {"dfrac", ""}, {"tfrac", ""}, {"binom", ""},
    {"sqrt", "the square root of"}, {"int", "the integral of"},
    {"iint", "the double integral of"}, {"iiint", "the triple integral of"},
    {"oint", "the contour integral of"},
    {"sum", "the sum of"}, {"prod", "the product of"},
    {"lim", "the limit of"}, {"limsup", "the limit superior of"},
    {"liminf", "the limit inferior of"},
    {"log", "log"}, {"ln", "natural log"}, {"lg", "log base 2"},
    {"exp", "e to the power"}, {"sin", "sine"}, {"cos", "cosine"},
    {"tan", "tangent"}, {"cot", "cotangent"}, {"sec", "secant"},
    {"csc", "cosecant"}, {"arcsin", "arc sine"}, {"arccos", "arc cosine"},
    {"arctan", "arc tangent"}, {"sinh", "hyperbolic sine"},
    {"cosh", "hyperbolic cosine"}, {"tanh", "hyperbolic tangent"},
    {"det", "the determinant of"}, {"dim", "the dimension of"},
    {"gcd", "the greatest common divisor of"}, {"deg", "degrees"},
    {"ker", "the kernel of"}, {"mod", "modulo"},
    // Delimiters and structure.
    {"left", ""}, {"right", ""}, {"langle", "angle bracket"},
    {"rangle", "angle bracket"}, {"lfloor", "floor"}, {"rfloor", "floor"},
    {"lceil", "ceiling"}, {"rceil", "ceiling"}, {"lbrace", "brace"},
    {"rbrace", "brace"}, {"vert", "vertical bar"},
    {"mid", "divides"}, {"quad", " "}, {"qquad", " "},
    {"text", ""}, {"mathrm", ""}, {"mathbf", ""}, {"mathit", ""},
    {"operatorname", ""}, {"pmod", "modulo"}, {"bmod", "modulo"},
    // Accents and decorations, spoken as the base character.
    {"hat", ""}, {"widehat", ""}, {"bar", ""}, {"overline", ""},
    {"tilde", ""}, {"widetilde", ""}, {"vec", ""}, {"dot", ""},
    {"ddot", ""}, {"underline", ""}, {"overline", ""},
    // Spacing and grouping.
    {"left.", ""}, {"right.", ""}, {"mathstrut", ""}, {"vphantom", ""},
    {"phantom", ""}, {"hphantom", ""},
    // Environments, which a reader hears as "begin aligned".
    {"begin", "begin"}, {"end", "end"},
});

/// Commands that read as a word rather than as a symbol, and therefore take
/// their argument directly rather than being read in symbol order.
bool isFunction(std::string_view name) {
  return name == "frac" || name == "dfrac" || name == "tfrac" || name == "binom" || name == "sqrt" ||
      name == "pmod" || name == "bmod" || name == "text" || name == "operatorname";
}

std::string_view trim(std::string_view text) {
  size_t start = 0;
  size_t end = text.size();
  while (start < end && std::isspace(static_cast<unsigned char>(text[start]))) {
    start++;
  }
  while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
    end--;
  }
  return text.substr(start, end - start);
}

/// A spoken form, built in pieces.
class Utterance {
public:
  void say(std::string_view words) {
    if (words.empty()) {
      return;
    }
    if (words == " ") {
      space();
      return;
    }
    if (buffer_.empty() || endsWithSpace_) {
      buffer_.append(words);
      endsWithSpace_ = false;
      return;
    }
    buffer_.push_back(' ');
    buffer_.append(words);
  }

  void space() {
    if (!buffer_.empty() && !endsWithSpace_) {
      buffer_.push_back(' ');
      endsWithSpace_ = true;
    }
  }

  /// A number, read as one value rather than digit by digit, which is what a
  /// reader means by "42" and what KaTeX's MathML produces.
  void number(std::string_view digits) {
    if (digits.empty()) {
      return;
    }
    space();
    buffer_.append(digits);
    endsWithSpace_ = false;
  }

  const std::string &text() const { return buffer_; }

private:
  bool endsWithSpace_{false};
  std::string buffer_;
};

class Reader {
public:
  Reader(std::string_view tex, Utterance &out) : tex_(tex), out_(out) {}

  void run() {
    while (offset_ < tex_.size()) {
      const char c = tex_[offset_];
      if (c == '\\') {
        command();
        continue;
      }
      if (c == '{' || c == '}') {
        // A group is read as its contents: `\frac{a}{b}` says "a over b" and
        // `\left(x\right)` says "x", not "left x right".
        offset_++;
        continue;
      }
      if (std::isspace(static_cast<unsigned char>(c))) {
        offset_++;
        continue;
      }
      if (std::isdigit(static_cast<unsigned char>(c))) {
        number();
        continue;
      }
      if (c == '^' || c == '_') {
        script(c);
        continue;
      }
      if (c == '&' || c == '\\') {
        // A row or column separator inside an environment.
        offset_++;
        out_.say("and");
        continue;
      }
      operator_();
    }
  }

private:
  void number() {
    const size_t start = offset_;
    while (offset_ < tex_.size() && (std::isdigit(static_cast<unsigned char>(tex_[offset_])) || tex_[offset_] == '.')) {
      offset_++;
    }
    out_.number(tex_.substr(start, offset_ - start));
  }

  /// A command: a named one is spoken, and an unknown one is spoken as its own
  /// name. Speaking the name is the point: an unsupported construct that is
  /// silent would read as a formula that stopped mid-sentence.
  void command() {
    offset_++;
    std::string name;
    if (offset_ < tex_.size() && !std::isalpha(static_cast<unsigned char>(tex_[offset_]))) {
      // A single-character command: `\,` is a space, `\%` is a percent sign.
      const char single = tex_[offset_++];
      switch (single) {
        case ',': case ';': case ' ': case ':': case '!': out_.space(); return;
        case '%': out_.say("percent"); return;
        case '{': case '}': return;
        case '&': out_.say("and"); return;
        case '#': out_.say("sharp"); return;
        default: out_.say(std::string(1, single)); return;
      }
    }
    while (offset_ < tex_.size() && std::isalpha(static_cast<unsigned char>(tex_[offset_]))) {
      name.push_back(tex_[offset_++]);
    }
    if (name.empty()) {
      return;
    }
    const std::string_view spoken = commandName(name);
    if (isFunction(name)) {
      if (name == "frac" || name == "dfrac" || name == "tfrac" || name == "binom") {
        // Two arguments: "a over b", which is what a reader expects and what
        // "a divided by b" would be wrong about.
        const std::string first = argument();
        const std::string second = argument();
        if (first.empty()) {
          return;
        }
        out_.say(first);
        if (!second.empty()) {
          out_.say(name == "binom" ? "choose" : "over");
          out_.say(second);
        }
        return;
      }
      if (name == "sqrt") {
        out_.say("the square root of");
        const std::string first = argument();
        if (!first.empty()) {
          out_.say(first);
        }
        if (peek() == '[') {
          const std::string index = argument();
          if (!index.empty()) {
            out_.say("at");
            out_.say(index);
          }
        }
        return;
      }
      const std::string argumentText = argument();
      if (!spoken.empty()) {
        out_.say(spoken);
      }
      if (!argumentText.empty()) {
        out_.say(argumentText);
      }
      return;
    }
    if (!spoken.empty()) {
      out_.say(spoken);
      return;
    }
    if (name == "begin" || name == "end") {
      const std::string environment = argument();
      if (name == "begin") {
        out_.say("begin");
      }
      if (!environment.empty()) {
        out_.say(environment);
      }
      if (name == "end") {
        out_.say("end");
      }
      return;
    }
    // An unknown command is spoken as its name without the backslash, so the
    // reader hears what was written and can decide to read the source.
    out_.say(name);
  }

  /// A superscript or subscript, as the phrase a reader expects.
  void script(char marker) {
    offset_++;
    const bool superscript = marker == '^';
    if (peek() == '{') {
      const std::string inner = argument();
      if (inner.empty()) {
        return;
      }
      if (superscript) {
        if (inner == "2") {
          out_.say("squared");
        } else if (inner == "3") {
          out_.say("cubed");
        } else if (inner == "0" || inner == "1") {
          return;
        } else {
          out_.say("to the power");
          out_.say(inner);
        }
      } else {
        out_.say("subscript");
        out_.say(inner);
      }
      return;
    }
    const size_t start = offset_;
    if (offset_ < tex_.size()) {
      offset_++;
    }
    const std::string single(tex_.substr(start, offset_ - start));
    if (superscript) {
      if (single == "2") {
        out_.say("squared");
      } else if (single == "3") {
        out_.say("cubed");
      } else {
        out_.say("to the power");
        out_.say(single);
      }
    } else {
      out_.say("subscript");
      out_.say(single);
    }
  }

  /// One braced or single-character argument, spoken.
  std::string argument() {
    skipSpace();
    if (peek() != '{') {
      if (offset_ >= tex_.size()) {
        return {};
      }
      const char c = tex_[offset_];
      if (c == '\\') {
        const size_t before = offset_;
        command();
        return std::string(trim(tex_.substr(before, offset_ - before)));
      }
      offset_++;
      return std::string(1, c);
    }
    const size_t end = tex_.find('}', offset_);
    if (end == std::string_view::npos) {
      const std::string rest(tex_.substr(offset_));
      offset_ = tex_.size();
      Utterance nested;
      Reader inner(rest, nested);
      inner.run();
      return nested.text();
    }
    const std::string_view body = tex_.substr(offset_ + 1, end - offset_ - 1);
    offset_ = end + 1;
    Utterance nested;
    Reader inner(body, nested);
    inner.run();
    return nested.text();
  }

  /// A symbol or a letter, as itself.
  void operator_() {
    const size_t start = offset_;
    // A run of letters is a word, not a string of letters: `ab` is "a b" in
    // math, but a formula's own identifiers read better as words, and a screen
    // reader would say "a b" either way.
    if (std::isalpha(static_cast<unsigned char>(tex_[start]))) {
      size_t end = start;
      while (end < tex_.size() && std::isalpha(static_cast<unsigned char>(tex_[end]))) {
        end++;
      }
      const std::string_view word = tex_.substr(start, end - start);
      offset_ = end;
      out_.say(word);
      return;
    }
    if (std::isalnum(static_cast<unsigned char>(tex_[start]))) {
      offset_ = start + 1;
      return;
    }
    offset_ = start + 1;
    switch (tex_[start]) {
      case '=': out_.say("equals"); break;
      case '+': out_.say("plus"); break;
      case '-': out_.say("minus"); break;
      case '*': out_.say("times"); break;
      case '/': out_.say("divided by"); break;
      case '<': out_.say("is less than"); break;
      case '>': out_.say("is greater than"); break;
      case '!': break;
      case ',': out_.say("comma"); break;
      case '.': out_.say("point"); break;
      case '(': out_.say("open bracket"); break;
      case ')': out_.say("close bracket"); break;
      case '[': out_.say("open bracket"); break;
      case ']': out_.say("close bracket"); break;
      case '|': out_.say("bar"); break;
      case ':': out_.say("colon"); break;
      case ';': out_.say("semicolon"); break;
      case '~': out_.say("space"); break;
      default: break;
    }
  }

  char peek() const { return offset_ < tex_.size() ? tex_[offset_] : '\0'; }
  void skipSpace() {
    while (offset_ < tex_.size() && std::isspace(static_cast<unsigned char>(tex_[offset_]))) {
      offset_++;
    }
  }

  std::string_view tex_;
  Utterance &out_;
  size_t offset_{0};
};

/// Collapses the runs of spaces a walk leaves behind, so a spoken form has one
/// space between words and none at either end.
std::string tidy(std::string text) {
  std::string out;
  out.reserve(text.size());
  bool space = false;
  for (const char c : text) {
    if (c == ' ') {
      space = !out.empty();
      continue;
    }
    if (space) {
      out.push_back(' ');
      space = false;
    }
    out.push_back(c);
  }
  return out;
}

} // namespace

std::string_view commandName(std::string_view command) {
  for (const auto &entry : kNames) {
    if (entry.first == command) {
      return entry.second;
    }
  }
  return {};
}

std::string speak(std::string_view tex, bool display) {
  const std::string_view body = trim(tex);
  if (body.empty()) {
    return display ? "math, empty" : "math, empty";
  }
  Utterance out;
  out.say("math");
  Reader reader(body, out);
  reader.run();
  std::string spoken = tidy(out.text());
  if (spoken.size() <= 4) {
    // A formula that produced nothing recognizable — a single unmatched brace,
    // say — reads better as its own source, which is the fallback the caller
    // would have used anyway.
    return std::string(tex);
  }
  return spoken;
}

} // namespace downstream::math
