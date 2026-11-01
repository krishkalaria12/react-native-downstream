#include "NativeLexer.h"

#include "HighlightsModule.h"
#include "IdentifierClasses.h"

#include <atomic>
#include <cstring>

#if !defined(__BYTE_ORDER__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "the lexer module's memory is little-endian and is read in place"
#endif

namespace {

/// Decodes the UTF-8 sequence of `length` bytes the module points at, or
/// returns a value past the last code point when it is out of bounds.
uint32_t pointAt(const w2c_env *env, uint32_t at, uint32_t length) {
  const wasm_rt_memory_t *memory = env->memory;
  if (length == 0 || length > 4 || at > memory->size || length > memory->size - at) {
    return 0x110000;
  }
  const uint8_t *p = memory->data + at;
  switch (length) {
  case 1:
    return p[0];
  case 2:
    return ((p[0] & 0x1fu) << 6) | (p[1] & 0x3fu);
  case 3:
    return ((p[0] & 0x0fu) << 12) | ((p[1] & 0x3fu) << 6) | (p[2] & 0x3fu);
  default:
    return ((p[0] & 0x07u) << 18) | ((p[1] & 0x3fu) << 12) | ((p[2] & 0x3fu) << 6) | (p[3] & 0x3fu);
  }
}

} // namespace

// The module's imports. It asks only about non-ASCII code points in
// ECMAScript identifiers.
extern "C" uint32_t w2c_env_is_id_start(struct w2c_env *env, uint32_t at, uint32_t length) {
  return downstream::isIdentifierStart(pointAt(env, at, length)) ? 1 : 0;
}

extern "C" uint32_t w2c_env_is_id_continue(struct w2c_env *env, uint32_t at, uint32_t length) {
  return downstream::isIdentifierContinue(pointAt(env, at, length)) ? 1 : 0;
}

namespace downstream {
namespace {

// The module's control block, in its first memory page.
constexpr uint32_t kLanguageAt = 0;
constexpr uint32_t kModeAt = 1;
constexpr uint32_t kInputLengthAt = 2;
constexpr uint32_t kOutputStartAt = 6;
constexpr uint32_t kOutputLengthAt = 10;
/// Output mode 3: (endUtf16, tokenId) records, split at line terminators.
constexpr uint8_t kLineRecords = 3;
/// Input starts on the second page. The module reads a NUL sentinel after it
/// and may look further ahead into reserved slack.
constexpr uint32_t kInputAt = 65536;
constexpr uint32_t kInputSlack = 96;
constexpr uint64_t kPageBytes = 65536;
/// Memory a thread's instance may keep between blocks. One long line can grow
/// it much further, and that much is not held for the life of the thread.
constexpr uint64_t kRetainedPages = 16;

std::atomic<uint64_t> gCreated{0};

uint32_t readU32(const uint8_t *at) {
  uint32_t value;
  std::memcpy(&value, at, sizeof(value));
  return value;
}

void writeU32(uint8_t *at, uint32_t value) { std::memcpy(at, &value, sizeof(value)); }



} // namespace

struct NativeLexerModule {
  HighlightsModule instance{};
  w2c_env env{};
  bool live{false};
  uint32_t imageMax{0};

  ~NativeLexerModule() {
    if (live) {
      wasm2c_downstream__highlights_free(&instance);
    }
  }
  wasm_rt_memory_t &memory() { return *w2c_downstream__highlights_memory(&instance); }
};

NativeLexer::NativeLexer() : module_(std::make_unique<NativeLexerModule>()) {
  static std::atomic<bool> initialized{false};
  if (!initialized.exchange(true)) {
    wasm_rt_init();
  }
  module_->env.memory = nullptr;
  if (downstream_highlights_instantiate(&module_->instance, &module_->env) != WASM_RT_TRAP_NONE) {
    return;
  }
  module_->live = true;
  module_->env.memory = &module_->memory();
  module_->imageMax = w2c_downstream__highlights_streamImageMax(&module_->instance);
  gCreated.fetch_add(1, std::memory_order_relaxed);
}

NativeLexer::~NativeLexer() = default;

bool NativeLexer::reusable() const { return module_->live && module_->memory().pages <= kRetainedPages; }

uint64_t NativeLexer::created() { return gCreated.load(std::memory_order_relaxed); }

bool NativeLexer::reserve(size_t bytes) {
  wasm_rt_memory_t &memory = module_->memory();
  if (bytes <= memory.size) {
    return true;
  }
  const uint64_t pages = (bytes - memory.size + kPageBytes - 1) / kPageBytes;
  return wasm_rt_grow_memory(&memory, pages) != static_cast<uint64_t>(-1);
}

bool NativeLexer::start(uint8_t language, const std::vector<uint8_t> *image) {
  if (!reserve(kInputAt + module_->imageMax + kInputSlack)) {
    return false;
  }
  uint8_t *data = module_->memory().data;
  data[kLanguageAt] = language;
  reset_ = image == nullptr;
  if (reset_) {
    return downstream_highlights_clear(&module_->instance) == WASM_RT_TRAP_NONE;
  }
  if (image->size() > module_->imageMax) {
    return false;
  }
  std::memcpy(data + kInputAt, image->data(), image->size());
  return downstream_highlights_restore(&module_->instance, kInputAt, static_cast<uint32_t>(image->size())) ==
      WASM_RT_TRAP_NONE;
}

bool NativeLexer::lex(std::string_view bytes, std::vector<uint32_t> *records) {
  records->clear();
  if (bytes.size() > UINT32_MAX - kInputAt - kInputSlack || !reserve(kInputAt + bytes.size() + kInputSlack)) {
    return false;
  }
  uint8_t *data = module_->memory().data;
  std::memcpy(data + kInputAt, bytes.data(), bytes.size());
  data[kInputAt + bytes.size()] = 0;
  data[kModeAt] = kLineRecords;
  writeU32(data + kInputLengthAt, static_cast<uint32_t>(bytes.size()));
  if (downstream_highlights_stream(&module_->instance, reset_ ? 1 : 0) != WASM_RT_TRAP_NONE) {
    return false;
  }
  reset_ = false;
  // The call may have grown, and so moved, the memory.
  const wasm_rt_memory_t &memory = module_->memory();
  const uint32_t start = readU32(memory.data + kOutputStartAt);
  const uint32_t length = readU32(memory.data + kOutputLengthAt);
  if (start > memory.size || length > memory.size - start || length % 8 != 0) {
    return false;
  }
  records->resize(length / sizeof(uint32_t));
  std::memcpy(records->data(), memory.data + start, length);
  return true;
}

bool NativeLexer::capture(std::vector<uint8_t> *image) {
  if (!reserve(kInputAt + module_->imageMax + kInputSlack)) {
    return false;
  }
  uint32_t length = 0;
  if (downstream_highlights_capture(&module_->instance, kInputAt, &length) != WASM_RT_TRAP_NONE ||
      length > module_->imageMax) {
    return false;
  }
  const uint8_t *data = module_->memory().data;
  image->assign(data + kInputAt, data + kInputAt + length);
  return true;
}

} // namespace downstream
