#include "DocumentRegistry.h"

#include <algorithm>
#include <mutex>
#include <unordered_map>

namespace downstream {

namespace {

std::mutex registryMutex;
std::unordered_map<uint64_t, std::weak_ptr<const Document>> registry;
size_t pruneThreshold = 64;

} // namespace

void DocumentRegistry::add(const DocumentHandle &document) {
  if (!document) {
    return;
  }
  std::lock_guard<std::mutex> lock(registryMutex);
  registry[document->id] = document;
  if (registry.size() >= pruneThreshold) {
    for (auto it = registry.begin(); it != registry.end();) {
      it = it->second.expired() ? registry.erase(it) : std::next(it);
    }
    pruneThreshold = std::max<size_t>(64, registry.size() * 2);
  }
}

DocumentHandle DocumentRegistry::find(uint64_t id) {
  std::lock_guard<std::mutex> lock(registryMutex);
  const auto it = registry.find(id);
  return it == registry.end() ? nullptr : it->second.lock();
}

} // namespace downstream
