#include "MathEngine.h"

#include "MathDisplayList.h"

#include <atomic>
#include <cstring>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>

namespace downstream {

bool mathEngineFunctionsFromSymbols(const void *parseAndLayout, const void *freeDisplayList, const void *lastError,
                                    MathEngineFunctions *out) {
  if (parseAndLayout == nullptr || freeDisplayList == nullptr || lastError == nullptr || out == nullptr) {
    return false;
  }
  out->parseAndLayout =
      reinterpret_cast<MathEngineResult (*)(const char *, const MathEngineOptions *)>(const_cast<void *>(parseAndLayout));
  out->free = reinterpret_cast<void (*)(char *)>(const_cast<void *>(freeDisplayList));
  out->lastError = reinterpret_cast<const char *(*)(void)>(const_cast<void *>(lastError));
  return true;
}

} // namespace downstream

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
  /// Set by `installResolver` and cleared once it has run, whatever it found.
  MathEngineResolver resolver{nullptr};

  /// Least-recently-used layouts and failures, keyed on TeX and display mode.
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
  struct CachedResult {
    MathLayout::Handle handle;
    std::string error;
  };
  std::list<std::pair<Key, CachedResult>> entries;
  std::unordered_map<Key, decltype(entries)::iterator, KeyHash> index;
  /// An in-flight call may finish after clearCache or engine replacement.
  /// Its caller still gets the result, but it must not refill the new cache.
  uint64_t generation{0};

  void invalidateCache() {
    entries.clear();
    index.clear();
    generation++;
  }

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
  shared.resolver = nullptr;
  shared.installed = functions.parseAndLayout != nullptr && functions.free != nullptr;
  shared.invalidateCache();
}

void installResolver(MathEngineResolver resolve, std::string_view fontDir) {
  Store &shared = store();
  std::lock_guard<std::mutex> lock(shared.mutex);
  if (shared.installed || resolve == nullptr) {
    return;
  }
  shared.resolver = resolve;
  shared.fontDir.assign(fontDir);
  shared.invalidateCache();
}

bool available() {
  Store &shared = store();
  std::lock_guard<std::mutex> lock(shared.mutex);
  return shared.installed || shared.resolver != nullptr;
}

std::string fontDirectory() {
  Store &shared = store();
  std::lock_guard<std::mutex> lock(shared.mutex);
  return shared.fontDir;
}

void clearCache() {
  Store &shared = store();
  std::lock_guard<std::mutex> lock(shared.mutex);
  shared.invalidateCache();
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
  MathEngineFunctions functions{};
  uint64_t generation = 0;
  {
    std::lock_guard<std::mutex> lock(shared.mutex);
    if (!shared.installed && shared.resolver != nullptr) {
      // The first formula any document needs: load the engine now, once. A
      // resolver that finds nothing leaves the engine uninstalled, which every
      // later call answers the same way without trying again.
      MathEngineFunctions resolved{};
      const MathEngineResolver resolve = shared.resolver;
      shared.resolver = nullptr;
      if (resolve(&resolved) && resolved.parseAndLayout != nullptr && resolved.free != nullptr) {
        shared.functions = resolved;
        shared.installed = true;
      }
    }
    if (!shared.installed) {
      return refuse("No math engine is installed.");
    }
    if (const auto found = shared.index.find(key); found != shared.index.end()) {
      shared.entries.splice(shared.entries.begin(), shared.entries, found->second);
      const auto &cached = found->second->second;
      return cached.handle ? cached.handle : refuse(cached.error);
    }
    functions = shared.functions;
    generation = shared.generation;
  }

  // The engine is called without the lock. RaTeX keeps its error slot per thread,
  // so the worker pool lays formulas out in parallel; holding the cache lock
  // across a layout would make every worker wait on the slowest formula. Two
  // workers that miss on the same formula both lay it out, and the second insert
  // is dropped below.
  //
  // The default color is the sentinel: the engine colors anything the formula
  // did not color itself with it, and the decoder maps it back to
  // `kInheritColor`. That is what makes a layout independent of the theme, so a
  // dark-mode switch or a theme change never lays a formula out again.
  const MathEngineColor sentinel{0, 0, 0, 0};
  const MathEngineOptions options{sizeof(MathEngineOptions), display ? 1 : 0, &sentinel};
  shared.engineCalls.fetch_add(1, std::memory_order_relaxed);
  const MathEngineResult result = functions.parseAndLayout(key.tex.c_str(), &options);
  Store::CachedResult cached;
  if (result.errorCode != 0 || result.data == nullptr) {
    const char *message = functions.lastError != nullptr ? functions.lastError() : nullptr;
    // An engine's free function may overwrite its thread-local error slot.
    // Copy the message before releasing even a failed result's buffer.
    cached.error = message != nullptr && message[0] != '\0'
        ? message : "The math engine could not lay out this formula.";
    if (result.data != nullptr) {
      functions.free(result.data);
    }
  } else {
    auto decoded = std::make_shared<MathLayout>();
    const bool ok = math::decodeDisplayList(result.data, decoded.get(), &cached.error);
    functions.free(result.data);
    if (ok) {
      cached.handle = std::move(decoded);
    } else if (cached.error.empty()) {
      cached.error = "The math engine returned a layout this build cannot read.";
    }
  }

  std::lock_guard<std::mutex> lock(shared.mutex);
  if (generation != shared.generation) {
    return cached.handle ? cached.handle : refuse(cached.error);
  }
  if (const auto found = shared.index.find(key); found != shared.index.end()) {
    // Another worker finished the same formula meanwhile. Its result wins, so
    // every snapshot shares one layout object or failure message.
    shared.entries.splice(shared.entries.begin(), shared.entries, found->second);
    const auto &existing = found->second->second;
    return existing.handle ? existing.handle : refuse(existing.error);
  }
  shared.entries.emplace_front(std::move(key), std::move(cached));
  shared.index.emplace(shared.entries.front().first, shared.entries.begin());
  while (shared.entries.size() > kMaxCacheEntries) {
    shared.index.erase(shared.entries.back().first);
    shared.entries.pop_back();
  }
  const auto &inserted = shared.entries.front().second;
  return inserted.handle ? inserted.handle : refuse(inserted.error);
}

} // namespace downstream::MathEngine
