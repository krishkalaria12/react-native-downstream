#include "Unicode.h"

namespace downstream {

namespace {

/// Decodes one UTF-8 sequence at `index`. Returns the sequence length and
/// writes the scalar value, or returns 1 with U+FFFD for malformed input.
size_t decodeUtf8(std::string_view bytes, size_t index, uint32_t &codePoint) {
  const auto lead = static_cast<unsigned char>(bytes[index]);
  if (lead < 0x80) {
    codePoint = lead;
    return 1;
  }

  size_t length = 0;
  uint32_t value = 0;
  uint32_t minimum = 0;
  if ((lead & 0xE0) == 0xC0) {
    length = 2;
    value = lead & 0x1F;
    minimum = 0x80;
  } else if ((lead & 0xF0) == 0xE0) {
    length = 3;
    value = lead & 0x0F;
    minimum = 0x800;
  } else if ((lead & 0xF8) == 0xF0) {
    length = 4;
    value = lead & 0x07;
    minimum = 0x10000;
  } else {
    codePoint = kReplacementCharacter;
    return 1;
  }

  if (index + length > bytes.size()) {
    codePoint = kReplacementCharacter;
    return 1;
  }
  for (size_t i = 1; i < length; i++) {
    const auto next = static_cast<unsigned char>(bytes[index + i]);
    if ((next & 0xC0) != 0x80) {
      codePoint = kReplacementCharacter;
      return 1;
    }
    value = (value << 6) | (next & 0x3F);
  }

  if (value < minimum || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) {
    codePoint = kReplacementCharacter;
    return 1;
  }
  codePoint = value;
  return length;
}

} // namespace

bool isValidUtf8(std::string_view bytes) {
  size_t index = 0;
  while (index < bytes.size()) {
    uint32_t codePoint = 0;
    const size_t length = decodeUtf8(bytes, index, codePoint);
    if (codePoint == kReplacementCharacter && length == 1 &&
        static_cast<unsigned char>(bytes[index]) >= 0x80) {
      return false;
    }
    index += length;
  }
  return true;
}

void appendCodePoint(std::u16string &out, uint32_t codePoint) {
  if (codePoint == 0 || codePoint > 0x10FFFF || (codePoint >= 0xD800 && codePoint <= 0xDFFF)) {
    out.push_back(kReplacementCharacter);
  } else if (codePoint < 0x10000) {
    out.push_back(static_cast<char16_t>(codePoint));
  } else {
    const uint32_t value = codePoint - 0x10000;
    out.push_back(static_cast<char16_t>(0xD800 + (value >> 10)));
    out.push_back(static_cast<char16_t>(0xDC00 + (value & 0x3FF)));
  }
}

void appendUtf8(std::u16string &out, std::string_view bytes) {
  size_t index = 0;
  while (index < bytes.size()) {
    const auto lead = static_cast<unsigned char>(bytes[index]);
    if (lead < 0x80) {
      out.push_back(static_cast<char16_t>(lead));
      index++;
      continue;
    }
    uint32_t codePoint = 0;
    index += decodeUtf8(bytes, index, codePoint);
    appendCodePoint(out, codePoint);
  }
}

Utf8Step utf8StepAt(std::string_view bytes, size_t index) {
  uint32_t codePoint = 0;
  const size_t bytes_ = decodeUtf8(bytes, index, codePoint);
  return {bytes_, codePoint >= 0x10000 ? 2u : 1u};
}

uint32_t utf16Length(std::string_view bytes) {
  uint32_t units = 0;
  for (size_t index = 0; index < bytes.size();) {
    const Utf8Step step = utf8StepAt(bytes, index);
    units += step.units;
    index += step.bytes;
  }
  return units;
}

std::u16string utf8ToUtf16(std::string_view bytes) {  std::u16string out;
  out.reserve(bytes.size());
  appendUtf8(out, bytes);
  return out;
}

std::string utf16ToUtf8(std::u16string_view text) {
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size(); i++) {
    uint32_t codePoint = text[i];
    if (codePoint >= 0xD800 && codePoint <= 0xDBFF && i + 1 < text.size() && text[i + 1] >= 0xDC00 &&
        text[i + 1] <= 0xDFFF) {
      codePoint = 0x10000 + ((codePoint - 0xD800) << 10) + (text[i + 1] - 0xDC00);
      i++;
    } else if (codePoint >= 0xD800 && codePoint <= 0xDFFF) {
      codePoint = kReplacementCharacter;
    }

    if (codePoint < 0x80) {
      out.push_back(static_cast<char>(codePoint));
    } else if (codePoint < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
      out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    } else if (codePoint < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
      out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
      out.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    }
  }
  return out;
}

uint32_t Utf16OffsetMap::toUtf16(size_t byteOffset) {
  if (byteOffset > source_.size()) {
    byteOffset = source_.size();
  }
  if (byteOffset < byteCursor_) {
    byteCursor_ = 0;
    utf16Cursor_ = 0;
  }
  while (byteCursor_ < byteOffset) {
    uint32_t codePoint = 0;
    byteCursor_ += decodeUtf8(source_, byteCursor_, codePoint);
    utf16Cursor_ += codePoint >= 0x10000 ? 2 : 1;
  }
  return utf16Cursor_;
}

} // namespace downstream
