#pragma once

#include "ThemeSlots.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace downstream {

/// One resolved theme: every slot in the table, defaults filled in, plus the
/// font names its font slots point at.
///
/// Immutable once interned. Two equal slot lists share one Theme, so every
/// message in a chat built with the same theme holds the same object and a
/// theme change costs one intern rather than one per view.
class Theme {
public:
  /// One color slot's resolved state.
  ///
  /// `set` is false while nothing overrides the slot, and the platform then
  /// draws `defaultRole` instead of a value: a platform color on iOS, the
  /// engine's token table for a syntax slot, or nothing at all for a role that
  /// means "inherit". That is why a default is a role rather than an integer.
  struct Color {
    bool set{false};
    uint32_t light{0};
    uint32_t dark{0};
    /// A `theme::ColorRole`, or `theme::kRoleTokenClassBase + a TokenClass`.
    uint16_t defaultRole{0};
  };

  using Handle = std::shared_ptr<const Theme>;

  /// Builds a theme with every default filled in and `values` applied at
  /// `slots`. The only way one is constructed, so the defaults are set from the
  /// one generated table in one place rather than from a list beside it.
  /// `slots` must be sorted ascending and unique, which is what `src/theme.ts`
  /// sends, and `values` is parallel to it.
  static Handle create(const std::vector<uint32_t> &slots,
                       const std::vector<double> &values,
                       const std::vector<std::string> &fonts,
                       uint64_t id,
                       uint64_t layoutKey);

  /// The empty theme, which is what an app that passes neither prop gets. Its
  /// `id` is 0, so a cache key that includes it is the key the same layout had
  /// before theming existed.
  static const Handle &defaultTheme();

  // Every accessor takes a slot id from `theme::Slot`. The ids are dense from
  // one, which is what lets each of these be one index rather than a lookup.

  /// A metric, weight, or flag as a number, in points before font scaling.
  double number(uint32_t id) const { return numbers_[id - 1]; }
  bool flag(uint32_t id) const { return numbers_[id - 1] != 0; }

  /// Whether the theme named this slot, as against leaving it at its default.
  ///
  /// The two are the same number and mean different things, and a caller has to
  /// be able to tell them: a `strong` whose weight slot is unset raises the
  /// block's own weight so a bold heading still reads as bolder, while one that
  /// named a weight means exactly that weight; a radius slot left at its default
  /// falls back to the shared `radius`, and one that named a value is final.
  /// A color has always answered this through `Color::set`.
  bool numberSet(uint32_t id) const { return (numberMask_[id - 1] & kSetMask) != 0; }

  /// True for any slot the theme named, whatever kind it is.
  bool slotSet(uint32_t id) const {
    if (theme::kSlotKinds[id - 1] == static_cast<uint8_t>(theme::SlotKind::Color)) {
      return color(id).set;
    }
    if (theme::kSlotKinds[id - 1] == static_cast<uint8_t>(theme::SlotKind::Font)) {
      return fonts_[id - 1] >= 0;
    }
    return numberSet(id);
  }

  /// One color's resolved state. Either half of a pair's two ids returns it, so
  /// a platform reading the id it was handed does not have to know which half
  /// the theme happened to send.
  const Color &color(uint32_t id) const {
    const uint32_t light = theme::kSlotLight[id - 1];
    return colors_[light == 0 ? id - 1 : light - 1];
  }

  /// The font family a font slot names, or an empty string for the platform's
  /// system font. The string outlives the theme.
  const std::string &font(uint32_t id) const {
    const int32_t index = fonts_[id - 1];
    return index < 0 ? kNoFont : fontNames_[index];
  }

  /// Every family the theme names, in slot order. A platform that has to build
  /// its own font objects reads one string per family rather than a string per
  /// font slot, which is what `ThemeSlots.h`'s heading tables would otherwise
  /// make it do.
  const std::vector<std::string> &fontNames() const { return fontNames_; }

  /// A hash over every slot. Two themes with the same content share an id.
  uint64_t id() const { return id_; }
  /// A hash over the slots that change size, and nothing else. A color-only
  /// change keeps it, so every measured size survives and Yoga sees no change.
  uint64_t layoutKey() const { return layoutKey_; }

private:
  friend class ThemeRegistry;

  Theme() = default;

  /// One number per slot id, holding a metric, a weight, or a flag's 0 or 1.
  /// Unused for a color or a font, whose resolved state is in the other arrays.
  std::vector<double> numbers_;
  /// One entry per slot id. A color slot's default role is kept here rather than
  /// in `numbers_`, so `number()` on a color slot cannot be mistaken for one.
  std::vector<Color> colors_;
  /// One index per slot id into `fontNames_`, or -1 for the system font.
  std::vector<int32_t> fonts_;
  /// One byte per slot id, with `kSetMask` set for a slot the theme named.
  /// A `std::vector<bool>` would be smaller, but a caller reads this per slot in
  /// the hottest loop in both renderers, and a byte test is the difference
  /// between one load and a bit extraction from a shared word.
  std::vector<uint8_t> numberMask_;
  static constexpr uint8_t kSetMask = 0x1;
  /// Every family the theme names, each interned once.
  std::vector<std::string> fontNames_;

  uint64_t id_{0};
  uint64_t layoutKey_{0};

  /// The name a font slot reads when it names no family: an empty string, which
  /// every platform already means as its system font.
  static const std::string kNoFont;
};

/// Hands out shared Theme objects by content, so equal themes are one object.
///
/// This is the only place a theme is ever looked up, which keeps the two hashes
/// and the interning pool in one place rather than spread across the three
/// places a theme is created.
class ThemeRegistry {
public:
  /// Returns the interned theme for these overrides, building it only when the
  /// content is new. `slots` must be sorted ascending and unique, which is what
  /// `src/theme.ts` sends; a repeated, descending, or out-of-range id leaves the
  /// default theme rather than a theme with half its values.
  ///
  /// `values` holds one entry per id in `slots`, and `fonts` the family names
  /// its font slots point at. Safe to call from any thread.
  static Theme::Handle intern(const std::vector<uint32_t> &slots,
                              const std::vector<double> &values,
                              const std::vector<std::string> &fonts);

  /// The interned theme with this id, or null when the pool does not hold it.
  ///
  /// This is what the platforms resolve a state by. The theme travels with the
  /// shadow node's state as its id, the way the document travels as its id, and
  /// the drawing layer looks the real theme up again rather than having every
  /// slot copied across the boundary on every update. A null answer means the
  /// pool never held the theme, which cannot happen while the node's state holds
  /// it, so a caller that gets null has an id from another build.
  static Theme::Handle find(uint64_t id);

  /// Drops the pool. An interned theme a handle still references stays alive;
  /// the pool simply stops being able to hand it out again. For tests.
  static void clear();
};

} // namespace downstream