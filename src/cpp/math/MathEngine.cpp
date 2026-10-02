#include "MathEngine.h"

#include "MathDisplayList.h"

#include <atomic>
#include <cstring>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>

namespace downstream::MathEngine {

namespace {

/// The installed engine and the cache, guarded together.
///
/// One lock rather than two because installation happens once, before any
/// worker starts, and a reader that had to take two locks in the right order
/// would be two chances to get it wrong for no benefit.
struct Store {
  std::mutex mutex;
  MathEngineFunctions functions{};
  std::string fontDir;
  bool installed{false};

  /// Least-recently-used layouts, keyed on the TeX and the display flag.
  ///
  /// A formula's layout is a function of its final TeX alone, so a full-mode
  /// reparse and a message that streamed in a thousand chunks call the engine
  /// exactly once between them. That is the whole reason the pass is cheap on a
  /// streaming snapshot, and it is why the cache is here rather than in the
  /// pass.
  struct Key {
    std::string tex;
    bool display{false};

    bool operator==(const Key &other) const { return display == other.display && tex == other.tex; }
  };
  struct KeyHash {
    size_t operator()(const Key &key) const {
      // FNV-1a over the TeX and the flag. A hash here only has to spread keys
      // across buckets, and correctness comes from operator==.
      size_t hash = 1469598103934665603ull;
      for (const char c : key.tex) {
        hash = (hash ^ static_cast<unsigned char>(c)) * 1099511628211ull;
      }
      hash = (hash ^ (key.display ? 1u : 0u)) * 1099511628211ull;
      return hash;
    }
  };
  std::list<std::pair<Key, MathLayout::Handle>> entries;
  std::unordered_map<Key, decltype(entries)::iterator, KeyHash> index;

  /// How many calls reached the engine. A budget test asserts this does not
  /// move for a document with no formula in it.
  std::atomic<uint64_t> engineCalls{0};
};

/// Deliberately leaked, so a worker that finishes a layout during teardown
/// cannot write to a destroyed map.
Store &store() {
  static auto *instance = new Store();
  return *instance;
}

/// A cache bounded by item count rather than by bytes.
///
/// A display list's size is not knowable before it is decoded, and a count
/// bound is what makes eviction cheap. 2,000 formulas is far more than one
/// answer holds, and the entries that do get evicted are the ones from a
/// document the reader has already scrolled past.
constexpr size_t kMaxCacheEntries = 2000;

} // namespace

void install(const MathEngineFunctions &functions, std::string_view fontDir) {
  Store &shared = store();
  std::lock_guard<std::mutex> lock(shared.mutex);
  shared.functions = functions;
  shared.fontDir.assign(fontDir);
  shared.installed = functions.parseAndLayout != nullptr && functions.free != nullptr;
}

bool available() {
  Store &shared = store();
  std::lock_guard<std::mutex> lock(shared.mutex);
  return shared.installed;
}

std::string_view fontDirectory() {
  Store &shared = store();
  std::lock_guard<std::mutex> lock(shared.mutex);
  return shared.fontDir;
}

void clearCache() {
  Store &shared = store();
  std::lock_guard<std::mutex> lock(shared.mutex);
  shared.entries.clear();
  shared.index.clear();
  shared.engineCalls.store(0, std::memory_order_relaxed);
}

uint64_t cachedCount() {
  Store &shared = store();
  std::lock_guard<std::mutex> lock(shared.mutex);
  return shared.entries.size();
}

uint64_t engineCallCount() { return store().engineCalls.load(std::memory_order_relaxed); }

MathLayout::Handle layout(std::string_view tex, bool display, std::string *error) {
  const auto refuse = [&](const std::string &message) -> MathLayout::Handle {
    if (error != nullptr && error->empty()) {
      *error = message;
    }
    return nullptr;
  };
  Store &shared = store();

  Store::Key key{std::string(tex), display};
  MathEngineOptions options;
  options.display = display ? 1 : 0;
  // The sentinel: the engine colors anything the formula did not color itself
  // with this, and the decoder maps it back to `kInheritColor`. That is what
  // makes a layout independent of the theme, so a dark-mode switch or a theme
  // change never re-lays a formula out.
  options.defaultColor = MathSentinelColor{0, 0, 0, 0};

  std::lock_guard<std::mutex> lock(shared.mutex);
  if (!shared.installed) {
    return refuse("No math engine is installed.");
  }
  if (const auto found = shared.index.find(key); found != shared.index.end()) {
    shared.entries.splice(shared.entries.begin(), shared.entries, found->second);
    return found->second->second;
  }

  options.fontDir = shared.fontDir.empty() ? nullptr : shared.fontDir.c_str();
  shared.engineCalls.fetch_add(1, std::memory_order_relaxed);
  const MathEngineResult result = shared.functions.parseAndLayout(key.tex.c_str(), &options);
  if (result.ok == 0 || result.json == nullptr) {
    const char *message = shared.functions.lastError != nullptr ? shared.functions.lastError() : nullptr;
    return refuse(message != nullptr && message[0] != '\0' ? message : "The math engine could not lay out this formula.");
  }

  auto decoded = std::make_shared<MathLayout>();
  std::string failure;
  const bool ok = math::decodeDisplayList(result.json, decoded.get(), &failure);
  shared.functions.free(const_cast<char *>(result.json));
  if (!ok) {
    return refuse(failure.empty() ? "The math engine returned a layout this build cannot read." : failure);
  }

  MathLayout::Handle handle = decoded;
  shared.entries.emplace_front(std::move(key), handle);
  shared.index.emplace(shared.entries.front().first, shared.entries.begin());
  while (shared.entries.size() > kMaxCacheEntries) {
    shared.index.erase(shared.entries.back().first);
    shared.entries.pop_back();
  }
  return handle;
}

} // namespace downstream::MathEngine
