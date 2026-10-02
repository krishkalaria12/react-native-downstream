#pragma once

// The platform's own image loader, handed to the engine once.
//
// The engine never creates an image loader. On iOS that is `RCTImageLoader` from
// the bridge's context container, which is the same object React Native's own
// `<Image>` reads, so the app's HTTP stack, its cookies, its disk cache, and any
// custom URL handlers all apply. On Android it is Fresco's pipeline, which the
// app initializes with its own configuration.
//
// A function pointer returning an opaque handle rather than a class, because the
// C++ engine has no way to name an Objective-C class or a Fresco pipeline, and
// the alternative — a small class per platform with a shared base — would be a
// vtable and a header for one function. The handle is never dereferenced here:
// only the platform code that installed it uses the value.

#include <cstdint>
#include <string>

namespace downstream {

class ImageLoaderRegistry {
public:
  /// What the platform's resolver returns: an opaque handle, or null when the
  /// platform has no loader yet. React Native's own pipeline is not available
  /// until the bridge is up, so null is a normal answer, not a failure.
  using Handle = void *;
  using Resolver = Handle (*)();

  /// Installs the platform's resolver. Called once per process, by the platform
  /// layer, and only the last call wins — which is what lets a view mount after
  /// the bridge is up replace an earlier answer.
  static void install(Resolver resolver);

  /// The platform's loader, or null.
  static Handle resolve();
};

} // namespace downstream
