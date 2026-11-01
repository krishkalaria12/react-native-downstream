#include "StreamLexer.h"
#include "DownstreamCodeHighlighter.h"

#include <mutex>

namespace downstream {
namespace {
struct Installed {
  std::mutex mutex;
  DownstreamCodeHighlighterFunctions functions{};
  CodeHighlighterResolver resolver{nullptr};
};
Installed &installed() {
  static auto *value = new Installed();
  return *value;
}

bool valid(const DownstreamCodeHighlighterFunctions *functions) {
  return functions != nullptr && functions->version == 1 && functions->size >= sizeof(*functions) &&
      functions->create != nullptr && functions->destroy != nullptr && functions->start != nullptr &&
      functions->lex != nullptr && functions->capture != nullptr && functions->reusable != nullptr &&
      functions->created != nullptr;
}

DownstreamCodeHighlighterFunctions functions() {
  auto &shared = installed();
  std::lock_guard<std::mutex> lock(shared.mutex);
  if (shared.functions.create == nullptr && shared.resolver != nullptr) {
    const auto resolver = shared.resolver;
    shared.resolver = nullptr;
    const auto *resolved = static_cast<const DownstreamCodeHighlighterFunctions *>(resolver());
    if (valid(resolved)) shared.functions = *resolved;
  }
  return shared.functions;
}
thread_local std::unique_ptr<StreamLexer> tLexer;
} // namespace

struct StreamLexerModule {
  DownstreamCodeHighlighterFunctions functions;
  void *instance{nullptr};
  explicit StreamLexerModule(DownstreamCodeHighlighterFunctions api) : functions(api), instance(api.create()) {}
  ~StreamLexerModule() { if (instance != nullptr) functions.destroy(instance); }
};

StreamLexer::StreamLexer() : module_(std::make_unique<StreamLexerModule>(functions())) {}
StreamLexer::~StreamLexer() = default;

StreamLexer *StreamLexer::current() {
  if (!codeHighlighterAvailable()) return nullptr;
  if (tLexer != nullptr && !tLexer->module_->functions.reusable(tLexer->module_->instance)) tLexer.reset();
  if (tLexer == nullptr) {
    tLexer.reset(new StreamLexer());
    if (tLexer->module_->instance == nullptr) tLexer.reset();
  }
  return tLexer.get();
}
uint64_t StreamLexer::created() {
  const auto api = functions();
  return api.created != nullptr ? api.created() : 0;
}
void StreamLexer::discard() { tLexer.reset(); }

bool StreamLexer::start(uint8_t language, const std::vector<uint8_t> *image) {
  return module_->functions.start(module_->instance, language,
      image != nullptr ? image->data() : nullptr, image != nullptr ? image->size() : 0, image == nullptr);
}
bool StreamLexer::lex(std::string_view bytes, std::vector<uint32_t> *records) {
  const uint32_t *data = nullptr;
  size_t count = 0;
  records->clear();
  if (!module_->functions.lex(module_->instance, bytes.data(), bytes.size(), &data, &count) ||
      (count > 0 && data == nullptr) || count % 2 != 0) return false;
  if (count > 0) records->assign(data, data + count);
  return true;
}
bool StreamLexer::capture(std::vector<uint8_t> *image) {
  const uint8_t *data = nullptr;
  size_t length = 0;
  image->clear();
  if (!module_->functions.capture(module_->instance, &data, &length) || (length > 0 && data == nullptr)) return false;
  if (length > 0) image->assign(data, data + length);
  return true;
}

bool codeHighlighterAvailable() { return functions().create != nullptr; }
void installCodeHighlighterResolver(CodeHighlighterResolver resolver) {
  auto &shared = installed();
  std::lock_guard<std::mutex> lock(shared.mutex);
  shared.resolver = resolver;
}
void installCodeHighlighter(const DownstreamCodeHighlighterFunctions *api) {
  if (!valid(api)) return;
  auto &shared = installed();
  std::lock_guard<std::mutex> lock(shared.mutex);
  shared.functions = *api;
  shared.resolver = nullptr;
}
} // namespace downstream

extern "C" void DownstreamInstallCodeHighlighter(const void *functions) {
  downstream::installCodeHighlighter(static_cast<const DownstreamCodeHighlighterFunctions *>(functions));
}
