#import "DSFonts.h"

#include <react/renderer/textlayoutmanager/RCTFontProperties.h>
#include <react/renderer/textlayoutmanager/RCTFontUtils.h>

#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

#import <React/RCTLog.h>

using namespace downstream;

namespace {

/// A CSS weight as the `UIFontWeight` UIKit takes.
///
/// React Native maps its own weights onto `UIFontWeight` the same way, so a
/// themed heading and a `<Text>` at the same weight are the same face.
UIFontWeight weightFor(double css) {
  if (css >= 900) return UIFontWeightHeavy;
  if (css >= 800) return UIFontWeightBlack;
  if (css >= 700) return UIFontWeightBold;
  if (css >= 600) return UIFontWeightSemibold;
  if (css >= 500) return UIFontWeightMedium;
  if (css >= 400) return UIFontWeightRegular;
  if (css >= 300) return UIFontWeightLight;
  if (css >= 200) return UIFontWeightThin;
  if (css >= 100) return UIFontWeightUltraLight;
  return UIFontWeightRegular;
}

RCTFontStyle styleFor(bool italic) {
  return italic ? RCTFontStyleItalic : RCTFontStyleNormal;
}

/// One cache key. The weight is kept as a double because it arrives as one, and
/// comparing the bits is cheaper and more exact than rounding to a float.
struct Key {
  std::string family;
  double size;
  double weight;
  uint8_t traits;

  bool operator==(const Key &other) const {
    return family == other.family && size == other.size && weight == other.weight &&
        traits == other.traits;
  }
};

struct KeyHash {
  size_t operator()(const Key &key) const {
    size_t hash = std::hash<std::string>{}(key.family);
    auto mix = [&hash](uint64_t value) {
      hash ^= static_cast<size_t>(value) + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
    };
    uint64_t sizeBits = 0;
    std::memcpy(&sizeBits, &key.size, sizeof(sizeBits));
    mix(sizeBits);
    uint64_t weightBits = 0;
    std::memcpy(&weightBits, &key.weight, sizeof(weightBits));
    mix(weightBits);
    mix(key.traits);
    return hash;
  }
};

std::mutex &cacheMutex() {
  static std::mutex instance;
  return instance;
}

/// One cache per trait combination, bounded so a screen full of sizes and
/// weights does not keep every one it ever drew alive.
template <size_t Capacity>
struct FontCache {
  std::unordered_map<Key, UIFont *, KeyHash> entries;

  UIFont *get(const Key &key, double weight, bool italic, bool monospaced, bool &hit) {
    const auto found = entries.find(key);
    if (found != entries.end()) {
      hit = true;
      return found->second;
    }
    UIFont *font = nil;
    if (monospaced && key.family.empty()) {
      // The platform's own monospace, which is what an unset mono font means.
      font = [UIFont monospacedSystemFontOfSize:key.size weight:weightFor(weight)];
    } else {
      // Fabric's own resolver, the one its text layout uses off the main thread,
      // so a themed face is found in exactly the places `<Text>` finds it: a
      // family `expo-font` loaded, or one linked through react-native.config.js.
      RCTFontProperties properties;
      properties.family = key.family.empty() ? nil : @(key.family.c_str());
      properties.size = key.size;
      // The size is already scaled. The resolver does not default the
      // multiplier, so leaving it NaN asserts in debug and draws a NaN-sized
      // face in release.
      properties.sizeMultiplier = 1.0;
      properties.weight = weightFor(weight);
      properties.style = styleFor(italic);
      font = RCTFontWithFontProperties(properties);
    }
    if (font != nil && entries.size() >= Capacity) {
      entries.clear();
    }
    entries.emplace(key, font);
    hit = false;
    return font;
  }
};

/// Families already reported, so a not-yet-loaded font warns once rather than on
/// every run it appears in.
std::mutex &warningMutex() {
  static std::mutex instance;
  return instance;
}

std::unordered_map<std::string, bool> &warnedFamilies() {
  static std::unordered_map<std::string, bool> instance;
  return instance;
}

} // namespace

UIFont *DSMakeFont(const std::string &family, CGFloat size, double weight, bool italic, bool monospaced) {
  // Two caches rather than one keyed by the trait bits, so a monospace request
  // and a proportional one never collide and each keeps its own entries.
  static FontCache<64> proportional;
  static FontCache<16> monospacedCache;
  Key key{family, size, weight, static_cast<uint8_t>((italic ? 1 : 0) | (monospaced ? 2 : 0))};

  std::lock_guard<std::mutex> lock(cacheMutex());
  bool hit = false;
  UIFont *font = monospaced ? monospacedCache.get(key, weight, italic, monospaced, hit)
                            : proportional.get(key, weight, italic, monospaced, hit);
  if (hit || family.empty() || font == nil) {
    return font;
  }
  // Outside the lock is not an option without a second lock ordering to prove,
  // so the warning goes out here. It is once per family for the process, which is
  // the point: a rendering pass that hit a missing family says so once rather
  // than once per inline run.
  DSFontIsFallback(family, font);
  return font;
}

BOOL DSFontIsFallback(const std::string &requested, UIFont *resolved) {
  if (requested.empty() || resolved == nil) {
    return NO;
  }
  // The family and the PostScript names are what the request has to match: a
  // family resolves to a specific face, and a PostScript name is what a bundled
  // or asset font registers under.
  const bool sameFamily =
      resolved.familyName.length > 0 &&
      [resolved.familyName compare:@(requested.c_str()) options:NSCaseInsensitiveSearch] == NSOrderedSame;
  const bool samePostScript =
      resolved.fontName.length > 0 &&
      [resolved.fontName compare:@(requested.c_str()) options:NSCaseInsensitiveSearch] == NSOrderedSame;
  if (sameFamily || samePostScript) {
    return NO;
  }

  std::lock_guard<std::mutex> lock(warningMutex());
  auto &warned = warnedFamilies();
  const auto found = warned.find(requested);
  if (found != warned.end() && found->second) {
    return YES;
  }
  warned[requested] = true;
  // Through RCTLog, so it reaches LogBox rather than only the console.
  RCTLogWarn(@"[react-native-downstream] the font family \"%s\" is not registered, so this text is drawn in %@. "
             @"Render text only after useFonts reports the fonts loaded, or link the family with react-native.config.js.",
             requested.c_str(), resolved.fontName ?: @"the system font");
  return YES;
}