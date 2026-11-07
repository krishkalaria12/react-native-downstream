#pragma once

// One laid-out formula, and the seam the math engine is installed through.
//
// RaTeX is not linked into the core library. It ships in
// `@react-native-downstream/math`, an opt-in package, so an app that does not
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

/// RaTeX's `RatexColor`: normalized floats. The engine passes all zeros as the
/// default color, and the decoder maps exactly that value to `kInheritColor`,
/// which is what makes a layout independent of the theme.
///
/// The three structs below are plain C structs, with no initializers, because
/// they cross a C ABI by value.
struct MathEngineColor {
  float r;
  float g;
  float b;
  float a;
};

/// RaTeX's `RatexOptions`, field for field.
///
/// Mirrored rather than included, so the core compiles without the package. The
/// layout is RaTeX's C ABI, and `structSize` is how RaTeX tells one version of
/// it from another: fields past the size a caller passes are ignored.
struct MathEngineOptions {
  /// `sizeof(MathEngineOptions)`.
  size_t structSize;
  /// 1 for display style, which is what puts `\sum`'s limits above and below.
  int displayMode;
  /// The default color, or null for black.
  const MathEngineColor *color;
};

/// RaTeX's `RatexResult`. `data` is owned by the engine and released with
/// `free`; a non-zero `errorCode` means `data` is null and `lastError` holds a
/// thread-local message, valid until the next call on the same thread.
struct MathEngineResult {
  char *data;
  int errorCode;
};

/// RaTeX's C ABI, as function pointers: `ratex_parse_and_layout`,
/// `ratex_free_display_list` and `ratex_get_last_error`, with exactly their
/// signatures, so the math package installs RaTeX's own functions with no shim.
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

/// RaTeX's three symbols as addresses, which is what `dlsym` hands Android and
/// what the iOS pod takes from its static archive, as the functions above.
///
/// One cast, in one place, for both platforms. The mirror's layout is held to
/// RaTeX's own header by `test_math_engine`, which installs through this too.
/// False when any address is null.
bool mathEngineFunctionsFromSymbols(const void *parseAndLayout, const void *freeDisplayList,
                                    const void *lastError, MathEngineFunctions *out);

/// Finds the engine's functions on first use. Returns false when the library is
/// not there, in which case every formula keeps its literal.
using MathEngineResolver = bool (*)(MathEngineFunctions *out);

namespace MathEngine {

/// Hands the engine its three functions and where its fonts are. Called once
/// per process, and only by the package that carries the binary.
///
/// `fontDir` is the platform's own path syntax: a bundle resource directory on
/// iOS, an asset prefix on Android. It is UTF-8 because that is what both
/// resolve to, and it is copied rather than referenced so the engine holds no
/// pointer into a bundle that could be released first.
void install(const MathEngineFunctions &functions, std::string_view fontDir);

/// Records that an engine is present without loading it yet. `resolve` runs once,
/// on the first formula a document needs laid out, which is how Android keeps a
/// 3 MB library out of a process that never shows math. `available()` is true
/// from this call on, because the package that registers a resolver is installed.
void installResolver(MathEngineResolver resolve, std::string_view fontDir);

/// Whether a formula can be laid out at all. False means every formula keeps
/// its literal and a development warning names the package to install, once.
bool available();

/// The font directory the engine was installed with, or empty. A copy, because a
/// later install may replace it.
std::string fontDirectory();

/**
 * Lays out one formula, through the shared cache.
 *
 * Never throws and never returns a half-decoded list: a formula RaTeX rejects,
 * a display list the decoder refuses, and a cache miss are all the same answer,
 * which is a null handle and a message in `error`.
 *
 * Successes and failures are cached on the TeX and the display flag, so a
 * full-mode reparse and a recycled message reuse the same result. Concurrent
 * cache misses may call the engine independently, without serializing workers.
 * This is the whole reason the pass is cheap on a streaming snapshot: a thousand
 * tokens of the same formula cost one call.
 */
MathLayout::Handle layout(std::string_view tex, bool display, std::string *error);

/// Empties the layout cache. For tests.
void clearCache();

/// How many results the cache holds, and how many engine calls it has made.
/// The second number is what a budget test asserts on: a prose-only document
/// must never increment it.
uint64_t cachedCount();
uint64_t engineCallCount();

} // namespace MathEngine

} // namespace downstream
