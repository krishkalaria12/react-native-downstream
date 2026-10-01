#pragma once

#import <UIKit/UIKit.h>

#ifdef __cplusplus
#include "theme/Theme.h"
#endif

NS_ASSUME_NONNULL_BEGIN

#ifdef __cplusplus
/// One font resolution, cached by family, size, weight and italic.
///
/// `makeFont` and the span setup run once per inline run, so a cache is the
/// difference between one font object per run and one per distinct request. It
/// lives beside the renderers rather than in the engine because turning a family
/// and a weight into a `UIFont` is platform work.
UIFont *DSMakeFont(const std::string &family, CGFloat size, double weight, bool italic, bool monospaced);

/// Whether a resolved font is a fallback for a family it did not know.
///
/// A family `expo-font` has not loaded yet resolves to the system font, exactly
/// as `<Text>` does, which would be silent otherwise. One warning per family, so
/// a misspelled or not-yet-loaded font is visible instead of puzzling.
BOOL DSFontIsFallback(const std::string &requested, UIFont *resolved);
#endif

NS_ASSUME_NONNULL_END