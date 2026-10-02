#pragma once

// One laid-out formula, and the seam the math engine is installed through.
//
// RaTeX is not linked into the core library. It ships in
// `react-native-downstream-math`, an opt-in package, so an app that does not
// want a 3 MB binary does not get one and an app that installs it links one
// engine. The surface between them is three C functions and a JSON string,
// which is what `MathEngineFunctions` names.
//
// Nothing in this header includes RaTeX's own header. That is deliberate: the
// core must compile without the package present, and a header that had to be
// found would make the engine unbuildable for every app that did not install
// it. The JSON protocol is the contract, and the decoder in
// `MathDisplayList.cpp` is written against the protocol document rather than
// against a type.
//
// The decoder is tolerant on purpose. RaTeX is pre-1.0 and its protocol says
// unknown fields and item types must be ignored, so a version bump that adds a
// font or an item degrades to the platform's own font or to a dropped item
// rather than to a crash. A formula RaTeX cannot lay out shows its source,
// which is KaTeX's own `throwOnError: false` behaviour.

#include "MathLayout.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace downstream {

/// The color the engine asks the math engine to use for anything the formula
/// did not color itself. The decoder maps exactly this value to
/// `kInheritColor`, which is what makes a layout independent of the theme.
struct MathSentinelColor {
  uint8_t r{0};
  uint8_t g{0};
  uint8_t b{0};
  uint8_t a{0};
};

/// What the engine asks for on one call.
///
/// A field struct rather than a JSON string so the ABI is a fixed layout this
/// file owns; RaTeX's own header would be a second, unpinned copy of it.
struct MathEngineOptions {
  /// Display style, which is what makes `\sum` put its limits above and below.
  int display{0};
  /// A scale hint. 1 leaves the em metrics alone, which is what both platforms
  /// want because the platform chooses the size at draw time.
  double fontSize{1.0};
  /// The sentinel, so an uncolored item comes back as `kInheritColor`.
  MathSentinelColor defaultColor{};
  /// A directory of the KaTeX fonts, UTF-8. The engine finds the metrics it
  /// needs from these files; a null or wrong path is a layout failure, not a
  /// crash.
  const char *fontDir{nullptr};
};

/// The result one call produced. `json` is owned by the engine and released with
/// `free`; `error` is a thread-local message, valid until the next call on the
/// same thread.
struct MathEngineResult {
  const char *json{nullptr};
  int ok{0};
};

/// RaTeX's C ABI, as function pointers.
///
/// Function pointers rather than a linked symbol so the core has no undefined
/// reference to the engine: iOS installs the three functions from the math
/// pod's `+load`, and Android resolves them with `dlopen` the first time a
/// document needs one. A link-time dependency would be the one thing that
/// cannot be optional.
struct MathEngineFunctions {
  MathEngineResult (*parseAndLayout)(const char *tex, const MathEngineOptions *options);
  void (*free)(char *json);
  const char *(*lastError)(void);
};

namespace MathEngine {

/// Hands the engine its three functions and where its fonts are. Called once
/// per process, and only by the package that carries the binary.
///
/// `fontDir` is the platform's own path syntax: a bundle resource directory on
/// iOS, an asset prefix on Android. It is UTF-8 because that is what both
/// resolve to, and it is copied rather than referenced so the engine holds no
/// pointer into a bundle that could be released first.
void install(const MathEngineFunctions &functions, std::string_view fontDir);

/// Whether a formula can be laid out at all. False means every formula keeps
/// its literal and a development warning names the package to install, once.
bool available();

/// The font directory the engine was installed with, or empty.
std::string_view fontDirectory();

/**
 * Lays out one formula, through the shared cache.
 *
 * Never throws and never returns a half-decoded list: a formula RaTeX rejects,
 * a display list the decoder refuses, and a cache miss are all the same answer,
 * which is a null handle and a message in `error`.
 *
 * Cached on the TeX and the display flag, so a full-mode reparse and a recycled
 * message never call the engine twice for a formula it has already laid out.
 * This is the whole reason the pass is cheap on a streaming snapshot: a thousand
 * tokens of the same formula cost one call.
 */
MathLayout::Handle layout(std::string_view tex, bool display, std::string *error);

/// Empties the layout cache. For tests.
void clearCache();

/// How many layouts the cache holds, and how many engine calls it has made.
/// The second number is what a budget test asserts on: a prose-only document
/// must never increment it.
uint64_t cachedCount();
uint64_t engineCallCount();

} // namespace MathEngine

} // namespace downstream
