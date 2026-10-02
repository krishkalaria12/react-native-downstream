#include "ImageSizeCache.h"

#include <list>
#include <mutex>
#include <unordered_map>

namespace downstream {

namespace {

struct Store {
  std::mutex mutex;
  /// Most recently recorded first, so the oldest entry is the back of the list
  /// and eviction is a pop rather than a scan.
  std::list<std::pair<std::string, CachedImageSize>> entries;
  std::unordered_map<std::string, decltype(entries)::iterator> index;
};

/// Deliberately leaked: a function-local static would be destroyed during static
/// destruction, and a load completing late — a request cancelled at teardown,
/// say — would then write to a destroyed map. A process that never runs this
/// destructor pays one small allocation.
Store &store() {
  static auto *instance = new Store();
  return *instance;
}

} // namespace

const CachedImageSize *ImageSizeCache::find(std::string_view url) {
  // The returned pointer is into the map, so it is only valid until the next
  // call that can insert. A caller reads the fields immediately and copies
  // them, which is why no handle type is needed and why the lock can be let go
  // at all.
  static thread_local CachedImageSize copy;
  Store &shared = store();
  std::lock_guard<std::mutex> lock(shared.mutex);
  const auto found = shared.index.find(std::string(url));
  if (found == shared.index.end()) {
    return nullptr;
  }
  copy = found->second->second;
  return &copy;
}

void ImageSizeCache::record(std::string_view url, CachedImageSize size) {
  Store &shared = store();
  std::lock_guard<std::mutex> lock(shared.mutex);
  const std::string key(url);
  const auto found = shared.index.find(key);
  if (found != shared.index.end()) {
    const CachedImageSize previous = found->second->second;
    // A weaker answer never replaces a stronger one. A second request for a URL
    // that has already loaded reports `Loading` until it finishes, and letting
    // that through would put a message's image back to its placeholder for as
    // long as the duplicate request ran.
    const bool weaker = size.state != ImageState::Loaded && previous.state == ImageState::Loaded;
    if (weaker) {
      return;
    }
    found->second->second = size;
    // Re-insert at the front so a re-recorded entry is not the next evicted.
    shared.entries.splice(shared.entries.begin(), shared.entries, found->second);
    return;
  }
  shared.entries.emplace_front(key, size);
  shared.index.emplace(key, shared.entries.begin());
  while (shared.entries.size() > kMaxEntries) {
    shared.index.erase(shared.entries.back().first);
    shared.entries.pop_back();
  }
}

void ImageSizeCache::forget(std::string_view url) {
  Store &shared = store();
  std::lock_guard<std::mutex> lock(shared.mutex);
  const auto found = shared.index.find(std::string(url));
  if (found == shared.index.end()) {
    return;
  }
  shared.entries.erase(found->second);
  shared.index.erase(found);
}

void ImageSizeCache::clear() {
  Store &shared = store();
  std::lock_guard<std::mutex> lock(shared.mutex);
  shared.entries.clear();
  shared.index.clear();
}

size_t ImageSizeCache::size() {
  Store &shared = store();
  std::lock_guard<std::mutex> lock(shared.mutex);
  return shared.entries.size();
}

ImageStateLayout ImageSizeCache::stateLayoutFor(const std::vector<uint32_t> &ordinals,
                                                const std::vector<std::string> &urls,
                                                const std::vector<bool> &loadable, bool enabled) {
  ImageStateLayout layout;
  layout.enabled = enabled;
  layout.entries.reserve(ordinals.size());
  for (size_t index = 0; index < ordinals.size(); index++) {
    ImageEntry entry;
    entry.ordinal = ordinals[index];
    // A URL this document cannot load has no cache entry and never will, so it
    // is not asked about: an index past the end is an empty URL, which is
    // `Pending` while the document streams and `Disabled` once it settles.
    const bool urlLoadable = index < urls.size() && index < loadable.size() && loadable[index];
    if (!urlLoadable) {
      entry.state = enabled ? ImageState::Pending : ImageState::Disabled;
      layout.entries.push_back(entry);
      continue;
    }
    if (const CachedImageSize *known = find(urls[index])) {
      entry.state = known->state;
      entry.pixelWidth = known->pixelWidth;
      entry.pixelHeight = known->pixelHeight;
    } else {
      entry.state = enabled ? ImageState::Loading : ImageState::Disabled;
    }
    layout.entries.push_back(entry);
  }
  return layout;
}

} // namespace downstream
