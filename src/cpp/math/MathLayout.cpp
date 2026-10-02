#include "MathLayout.h"

#include <array>

namespace downstream {

namespace {

/// RaTeX's font names, in its own order. The name is the wire format, so this
/// table is the mapping and `fontFromName` is the only thing that needs it;
/// adding a font is one entry here and one in `MathDisplayList.cpp`.
struct FontEntry {
  MathFont font;
  std::string_view name;
};

constexpr FontEntry kFonts[] = {
    {MathFont::MainRegular, "Main-Regular"},
    {MathFont::MainBold, "Main-Bold"},
    {MathFont::MainItalic, "Main-Italic"},
    {MathFont::MainBoldItalic, "Main-BoldItalic"},
    {MathFont::MathItalic, "Math-Italic"},
    {MathFont::MathBoldItalic, "Math-BoldItalic"},
    {MathFont::AMSRegular, "AMS-Regular"},
    {MathFont::CaligraphicRegular, "Caligraphic-Regular"},
    {MathFont::CaligraphicBold, "Caligraphic-Bold"},
    {MathFont::FrakturRegular, "Fraktur-Regular"},
    {MathFont::FrakturBold, "Fraktur-Bold"},
    {MathFont::SansSerifRegular, "SansSerif-Regular"},
    {MathFont::SansSerifBold, "SansSerif-Bold"},
    {MathFont::SansSerifItalic, "SansSerif-Italic"},
    {MathFont::ScriptRegular, "Script-Regular"},
    {MathFont::TypewriterRegular, "Typewriter-Regular"},
    {MathFont::Size1, "Size1-Regular"},
    {MathFont::Size2, "Size2-Regular"},
    {MathFont::Size3, "Size3-Regular"},
    {MathFont::Size4, "Size4-Regular"},
    {MathFont::System, "CJK-Regular"},
};

/// The file each face ships as, in the KaTeX font set. The names are KaTeX's,
/// so a font directory vendored from KaTeX loads without renaming, and the
/// PostScript names below are the ones CoreText and Android's Typeface find in
/// the file's `name` table.
struct FontFile {
  MathFont font;
  std::string_view file;
  std::string_view postScript;
};

constexpr FontFile kFontFiles[] = {
    {MathFont::MainRegular, "KaTeX_Main-Regular.ttf", "KaTeX_Main-Regular"},
    {MathFont::MainBold, "KaTeX_Main-Bold.ttf", "KaTeX_Main-Bold"},
    {MathFont::MainItalic, "KaTeX_Main-Italic.ttf", "KaTeX_Main-Italic"},
    {MathFont::MainBoldItalic, "KaTeX_Main-BoldItalic.ttf", "KaTeX_Main-BoldItalic"},
    {MathFont::MathItalic, "KaTeX_Math-Italic.ttf", "KaTeX_Math-Italic"},
    {MathFont::MathBoldItalic, "KaTeX_Math-BoldItalic.ttf", "KaTeX_Math-BoldItalic"},
    {MathFont::AMSRegular, "KaTeX_AMS-Regular.ttf", "KaTeX_AMS-Regular"},
    {MathFont::CaligraphicRegular, "KaTeX_Caligraphic-Regular.ttf", "KaTeX_Caligraphic-Regular"},
    {MathFont::CaligraphicBold, "KaTeX_Caligraphic-Bold.ttf", "KaTeX_Caligraphic-Bold"},
    {MathFont::FrakturRegular, "KaTeX_Fraktur-Regular.ttf", "KaTeX_Fraktur-Regular"},
    {MathFont::FrakturBold, "KaTeX_Fraktur-Bold.ttf", "KaTeX_Fraktur-Bold"},
    {MathFont::SansSerifRegular, "KaTeX_SansSerif-Regular.ttf", "KaTeX_SansSerif-Regular"},
    {MathFont::SansSerifBold, "KaTeX_SansSerif-Bold.ttf", "KaTeX_SansSerif-Bold"},
    {MathFont::SansSerifItalic, "KaTeX_SansSerif-Italic.ttf", "KaTeX_SansSerif-Italic"},
    {MathFont::ScriptRegular, "KaTeX_Script-Regular.ttf", "KaTeX_Script-Regular"},
    {MathFont::TypewriterRegular, "KaTeX_Typewriter-Regular.ttf", "KaTeX_Typewriter-Regular"},
    {MathFont::Size1, "KaTeX_Size1-Regular.ttf", "KaTeX_Size1-Regular"},
    {MathFont::Size2, "KaTeX_Size2-Regular.ttf", "KaTeX_Size2-Regular"},
    {MathFont::Size3, "KaTeX_Size3-Regular.ttf", "KaTeX_Size3-Regular"},
    {MathFont::Size4, "KaTeX_Size4-Regular.ttf", "KaTeX_Size4-Regular"},
    // Not a file: the platform's own face for a code point KaTeX has no glyph
    // for, which is a CJK character or an emoji inside a formula.
    {MathFont::System, "", ""},
};

} // namespace

std::string_view mathFontFileName(MathFont font) {
  for (const FontFile &entry : kFontFiles) {
    if (entry.font == font) {
      return entry.file;
    }
  }
  return {};
}

std::string_view mathFontPostScriptName(MathFont font) {
  for (const FontFile &entry : kFontFiles) {
    if (entry.font == font) {
      return entry.postScript;
    }
  }
  return {};
}

std::string_view mathFontName(MathFont font) {
  for (const FontEntry &entry : kFonts) {
    if (entry.font == font) {
      return entry.name;
    }
  }
  return "CJK-Regular";
}

} // namespace downstream
