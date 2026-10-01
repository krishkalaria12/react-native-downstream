#import "DSThemeColors.h"

#include "TokenPalette.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <unordered_map>

using namespace downstream;
using namespace downstream::theme;

namespace {

/// One `UIColor` for an ARGB integer in both appearances.
///
/// Dynamic rather than one color per appearance, so a drawing made under one
/// appearance is correct under the other without a rebuild. This is what lets a
/// theme's light and dark pair work with no React render when the reader flips
/// appearance.
UIColor *dynamicColor(uint32_t light, uint32_t dark) {
  auto channel = [](uint32_t argb, int shift) {
    return ((argb >> shift) & 0xff) / 255.0;
  };
  return [UIColor colorWithDynamicProvider:^UIColor *(UITraitCollection *traits) {
    const uint32_t argb =
        traits.userInterfaceStyle == UIUserInterfaceStyleDark ? dark : light;
    return [UIColor colorWithRed:channel(argb, 16)
                           green:channel(argb, 8)
                            blue:channel(argb, 0)
                           alpha:channel(argb, 24)];
  }];
}

/// The platform color a role names, which is what an unset slot draws.
///
/// These are the same semantic colors the renderers used before theming existed,
/// so a view with no theme is byte for byte what it was: increased contrast and
/// appearance are followed by the platform, not by a frozen integer.
UIColor *roleColor(ColorRole role) {
  switch (role) {
    case ColorRole::kRoleLabel:
      return UIColor.labelColor;
    case ColorRole::kRoleSecondaryLabel:
      return UIColor.secondaryLabelColor;
    case ColorRole::kRoleLink:
      return UIColor.linkColor;
    case ColorRole::kRoleTertiaryFill:
      return UIColor.tertiarySystemFillColor;
    case ColorRole::kRoleSeparator:
      return UIColor.separatorColor;
    case ColorRole::kRoleGray3:
      return UIColor.systemGray3Color;
    case ColorRole::kRoleBlue:
      return UIColor.systemBlueColor;
    case ColorRole::kRoleGreen:
      return UIColor.systemGreenColor;
    case ColorRole::kRolePurple:
      return UIColor.systemPurpleColor;
    case ColorRole::kRoleOrange:
      return UIColor.systemOrangeColor;
    case ColorRole::kRoleRed:
      return UIColor.systemRedColor;
    case ColorRole::kRoleSelection:
      return UIColor.tintColor;
    case ColorRole::kRoleClear:
      return UIColor.clearColor;
    case ColorRole::kRoleInherit:
      // Nothing is drawn and the enclosing block's color shows through.
      return nil;
  }
  return UIColor.labelColor;
}

/// A color the theme named for one appearance only. The named half draws the
/// theme's value and the other draws the slot's role, resolved for that
/// appearance, so `{ dark }` leaves light mode exactly as it was.
UIColor *halfColor(const Theme::Color &color) {
  UIColor *themed = dynamicColor(color.light, color.dark);
  // An "inherit" role has no color of its own to fall back to, and a block's
  // text color is what it inherits, so it falls back to the label color.
  UIColor *fallback = roleColor(static_cast<ColorRole>(color.defaultRole)) ?: UIColor.labelColor;
  const bool lightSet = color.lightSet;
  const bool darkSet = color.darkSet;
  return [UIColor colorWithDynamicProvider:^UIColor *(UITraitCollection *traits) {
    const bool night = traits.userInterfaceStyle == UIUserInterfaceStyleDark;
    const bool named = night ? darkSet : lightSet;
    UIColor *source = named ? themed : fallback;
    return [source resolvedColorWithTraitCollection:traits];
  }];
}

} // namespace

@interface DSThemeColors ()
- (instancetype)initWithTheme:(const downstream::Theme &)theme;
@end

@implementation DSThemeColors {
  const downstream::Theme *_theme;
  /// One resolved color per slot id, built once so a rendering reads a pointer
  /// rather than building a dynamic color per run.
  std::vector<UIColor *> _bySlot;
  /// The syntax colors, indexed by class, for the same reason.
  std::vector<UIColor *> _tokens;
  /// Which halves of each color slot the theme named: light in bit 0, dark in
  /// bit 1.
  std::vector<uint8_t> _halves;
}

+ (instancetype)colorsFor:(const downstream::Theme &)theme {
  // One object per theme id, so every view on a theme holds the same colors and
  // a theme change builds one table rather than one per view.
  static std::mutex mutex;
  static std::unordered_map<uint64_t, DSThemeColors *> byId;
  static DSThemeColors *sharedDefault;

  if (theme.id() == 0) {
    std::lock_guard<std::mutex> lock(mutex);
    if (sharedDefault == nil) {
      sharedDefault = [[DSThemeColors alloc] initWithTheme:theme];
    }
    return sharedDefault;
  }

  std::lock_guard<std::mutex> lock(mutex);
  const auto found = byId.find(theme.id());
  if (found != byId.end() && found->second != nil) {
    return found->second;
  }
  DSThemeColors *built = [[DSThemeColors alloc] initWithTheme:theme];
  // Bounded, so an app previewing themes in a list does not keep every value it
  // ever used alive. A theme a mounted view still holds is kept by that view.
  if (byId.size() >= 16) {
    byId.erase(byId.begin());
  }
  byId.emplace(theme.id(), built);
  return built;
}

- (instancetype)initWithTheme:(const downstream::Theme &)theme {
  if (self = [super init]) {
    _theme = &theme;
    // One entry per slot id, minus the zero that is not a slot. The high id of a
    // color's pair is populated by the same pass, because `Theme::color` resolves
    // either half to the pair's single entry.
    _bySlot.assign(kSlotIdCount - 1, nil);
    _tokens.assign(kTokenColorsCount, nil);
    _halves.assign(kSlotIdCount - 1, 0);

    for (uint32_t id = 1; id < kSlotIdCount; id++) {
      if (kSlotKinds[id - 1] != static_cast<uint8_t>(SlotKind::Color)) {
        continue;
      }
      const Theme::Color &color = theme.color(id);
      _halves[id - 1] = static_cast<uint8_t>((color.lightSet ? 1 : 0) | (color.darkSet ? 2 : 0));
      if (!color.set) {
        const uint16_t role = color.defaultRole;
        // A syntax color's default is a token class offset by the base, and a
        // role is not, which is what tells the two apart in one field.
        if (role >= kRoleTokenClassBase) {
          continue;
        }
        _bySlot[id - 1] = roleColor(static_cast<ColorRole>(role));
        continue;
      }
      // A syntax slot's unset half already holds the palette, so only a role
      // slot named for one appearance needs the role for the other.
      const bool oneHalf = !(color.lightSet && color.darkSet) && color.defaultRole < kRoleTokenClassBase;
      _bySlot[id - 1] = oneHalf ? halfColor(color) : dynamicColor(color.light, color.dark);
    }

    for (size_t token = 0; token < kTokenColorsCount; token++) {
      // `kSyntax` holds the light id of each class's slot, and `Theme::color`
      // resolves either half of a pair to the same entry, so a themed pair and
      // an unset one are both read through this one id.
      // An unset class draws the engine's palette, which the theme carries as
      // the unset value. Plain and Operator are the exception: unset, they keep
      // the code block's own color, so a themed block text color reaches them.
      const Theme::Color &color = theme.color(kSyntax[token]);
      const auto klass = static_cast<TokenClass>(token);
      const bool takesBlockColor = klass == TokenClass::Plain || klass == TokenClass::Operator;
      const bool drawsOwnColor = color.set || !takesBlockColor;
      _tokens[token] = drawsOwnColor ? dynamicColor(color.light, color.dark) : nil;
    }
  }
  return self;
}

- (UIColor *)colorForSlot:(uint32_t)slot {
  if (slot == 0 || slot >= _bySlot.size() + 1) {
    return nil;
  }
  return _bySlot[slot - 1];
}

- (UIColor *)colorForSlot:(uint32_t)slot fallback:(UIColor *)fallback {
  UIColor *color = [self colorForSlot:slot];
  return color != nil ? color : fallback;
}

- (UIColor *)colorForSlot:(uint32_t)slot namedOr:(UIColor *)fallback {
  const uint8_t named = slot == 0 || slot > _halves.size() ? 0 : _halves[slot - 1];
  UIColor *themed = named == 0 ? nil : [self colorForSlot:slot];
  if (themed == nil) {
    return fallback;
  }
  if (named == 3) {
    return themed;
  }
  return [UIColor colorWithDynamicProvider:^UIColor *(UITraitCollection *traits) {
    const bool night = traits.userInterfaceStyle == UIUserInterfaceStyleDark;
    const bool ownHalf = (named & (night ? 2 : 1)) != 0;
    UIColor *source = ownHalf ? themed : fallback;
    return [source resolvedColorWithTraitCollection:traits];
  }];
}

- (UIColor *)tokenColor:(downstream::TokenClass)token fallback:(UIColor *)fallback {
  const size_t index = static_cast<size_t>(token);
  if (index >= _tokens.size() || _tokens[index] == nil) {
    return fallback;
  }
  return _tokens[index];
}

- (UIColor *)selectionColor {
  return [self colorForSlot:kSlotSelectionColor fallback:UIColor.tintColor];
}

@end