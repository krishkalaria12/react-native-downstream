#pragma once

#import <UIKit/UIKit.h>

#ifdef __cplusplus
#include "theme/Theme.h"
#include "theme/ThemeSlots.h"
#endif

NS_ASSUME_NONNULL_BEGIN

/// One theme's platform colors, built once per theme id.
///
/// A resolved color is a dynamic `UIColor`, so a cached attributed string or a
/// drawn view follows the appearance without anything being rebuilt: the
/// platform resolves it against the trait collection when it draws. An unset
/// slot draws its role instead, which is how a color nothing overrode keeps
/// following increased contrast.
@interface DSThemeColors : NSObject

/// The colors for `theme`. Built once and shared, so every view on one theme
/// holds the same objects. Safe to call from any thread.
+ (instancetype)colorsFor:(const downstream::Theme &)theme;

/// The color for a slot. Returns nil for a role that means "inherit", so the
/// enclosing block's color shows through, which is what a nil attribute does.
- (nullable UIColor *)colorForSlot:(uint32_t)slot;

/// The color for a slot, or `fallback` when the slot has none. Used where the
/// drawing needs a color either way, such as a bar or a rule. A nil `fallback`
/// asks whether the slot resolves at all, which is how a theme's "no zebra
/// striping" is told from "transparent striping".
- (UIColor *)colorForSlot:(uint32_t)slot fallback:(nullable UIColor *)fallback;

/// The slot's color in each appearance whose half the theme named, and
/// `fallback` in each it did not. A slot named for both is its own color, and
/// one named for neither is `fallback`, so a block inside a quote keeps the
/// quote's color in an appearance the theme left alone.
- (UIColor *)colorForSlot:(uint32_t)slot namedOr:(UIColor *)fallback;

/// The color for one token class, or `fallback` when the theme leaves it unset.
/// A nil `fallback` means the token keeps the code block's own color, which is
/// what a plain run and an operator keep today.
- (UIColor *)tokenColor:(downstream::TokenClass)token fallback:(nullable UIColor *)fallback;

/// The selection highlight, which is the platform's own unless a theme sets it.
@property (nonatomic, readonly) UIColor *selectionColor;

@end

NS_ASSUME_NONNULL_END