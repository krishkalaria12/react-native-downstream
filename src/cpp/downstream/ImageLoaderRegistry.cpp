#include "ImageLoaderRegistry.h"

#include <atomic>

namespace downstream {

namespace {

/// One atomic rather than a lock: this is read on every image load and written
/// once, and there is no state to protect beyond the pointer itself.
std::atomic<ImageLoaderRegistry::Resolver> resolver{nullptr};

} // namespace

void ImageLoaderRegistry::install(Resolver installed) {
  resolver.store(installed, std::memory_order_release);
}

ImageLoaderRegistry::Handle ImageLoaderRegistry::resolve() {
  Resolver installed = resolver.load(std::memory_order_acquire);
  return installed != nullptr ? installed() : nullptr;
}

} // namespace downstream
