#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace downstream {

constexpr char16_t kReplacementCharacter = 0xFFFD;

/// True when the bytes are well-formed UTF-8 without surrogate code points.
bool isValidUtf8(std::string_view bytes);

/// Appends one Unicode scalar value. Invalid values append U+FFFD.
void appendCodePoint(std::u16string &out, uint32_t codePoint);

/// Decodes UTF-8 into UTF-16. Malformed sequences decode as U+FFFD.
void appendUtf8(std::u16string &out, std::string_view bytes);
class SnapshotText;
void appendUtf8(SnapshotText &out, std::string_view bytes);

/// One UTF-8 sequence: how many bytes it spans and how many UTF-16 code units
/// it occupies. A malformed or truncated sequence spans one byte and one unit,
/// which is what appendUtf8 gives it as U+FFFD, so stepping with this and
/// walking the bytes always agree.
struct Utf8Step {
  size_t bytes{1};
  uint32_t units{1};
};
Utf8Step utf8StepAt(std::string_view bytes, size_t index);

/// UTF-16 code units in `bytes`, which is what appendUtf8 would produce.
uint32_t utf16Length(std::string_view bytes);

std::u16string utf8ToUtf16(std::string_view bytes);
std::string utf16ToUtf8(std::u16string_view text);

/// Converts UTF-8 byte offsets in one source into UTF-16 offsets. Lookups are
/// linear from the previous lookup, so ascending queries cost O(source) total.
class Utf16OffsetMap {
public:
  explicit Utf16OffsetMap(std::string_view source) : source_(source) {}

  uint32_t toUtf16(size_t byteOffset);

private:
  std::string_view source_;
  size_t byteCursor_{0};
  uint32_t utf16Cursor_{0};
};

} // namespace downstream
