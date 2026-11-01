#include "DownstreamCodeHighlighter.h"
#include "NativeLexer.h"

#include <new>
#include <vector>

namespace {
struct Instance {
  downstream::NativeLexer lexer;
  std::vector<uint32_t> records;
  std::vector<uint8_t> image;
};
void *create() {
  try {
    auto *instance = new Instance();
    if (instance->lexer.reusable()) return instance;
    delete instance;
    return nullptr;
  } catch (...) { return nullptr; }
}
void destroy(void *value) { delete static_cast<Instance *>(value); }
bool start(void *value, uint8_t language, const uint8_t *image, size_t length, bool reset) {
  auto *instance = static_cast<Instance *>(value);
  if (reset) return instance->lexer.start(language, nullptr);
  instance->image.clear();
  if (length > 0) instance->image.assign(image, image + length);
  return instance->lexer.start(language, &instance->image);
}
bool lex(void *value, const char *bytes, size_t length, const uint32_t **records, size_t *count) {
  auto *instance = static_cast<Instance *>(value);
  if (!instance->lexer.lex(std::string_view(bytes, length), &instance->records)) return false;
  *records = instance->records.data();
  *count = instance->records.size();
  return true;
}
bool capture(void *value, const uint8_t **image, size_t *length) {
  auto *instance = static_cast<Instance *>(value);
  if (!instance->lexer.capture(&instance->image)) return false;
  *image = instance->image.data();
  *length = instance->image.size();
  return true;
}
bool reusable(void *value) { return static_cast<Instance *>(value)->lexer.reusable(); }
const DownstreamCodeHighlighterFunctions api{
  1, sizeof(DownstreamCodeHighlighterFunctions), create, destroy, start, lex, capture, reusable,
  downstream::NativeLexer::created,
};
} // namespace

extern "C" __attribute__((visibility("default"))) const void *DownstreamGetCodeHighlighter() { return &api; }
