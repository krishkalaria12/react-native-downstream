#include "Theme.h"

#include "TokenPalette.h"

#include <algorithm>
#include <cstring>
#include <list>
#include <mutex>
#include <unordered_map>

namespace downstream {

using namespace theme;

const std::string Theme::kNoFont = "";

namespace {

/// Interned themes no handle references any more, kept so a view cycling
/// through themes finds its own again. Bounded, because an app previewing
/// themes in a list would otherwise keep every value it ever used alive.
constexpr size_t kRetainedThemes = 16;

/// Folded into both hashes first, so a hash computed before a change to how a
/// theme is summarised cannot be mistaken for one computed after it. The value
/// only has to change when the summarisation does.
constexpr uint64_t kHashVersion = 2;

/// The widest integer a double holds exactly.
constexpr uint64_t kMaxExactDouble = (1ull << 53) - 1;

/// FNV-1a, 64 bit. One round over a theme's slots, twice: once over everything
/// for `id` and once over the size-changing slots for `layoutKey`. Neither hash
/// guards anything, so both only have to be stable within a process and cheap.
class Hasher {
public:
  void add(uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
      state_ ^= (value >> shift) & 0xff;
      state_ *= 0x100000001b3ull;
    }
    count_++;
  }

  void add(const std::string &value) {
    add(value.size());
    for (const unsigned char byte : value) {
      state_ ^= byte;
      state_ *= 0x100000001b3ull;
    }
  }

  uint64_t digest() const {
    // The number of values folded in goes in too, so a shorter list cannot hash
    // the same as a longer one that starts the same way.
    // Kept to 53 bits, because the id crosses into Android as a double in the
    // Fabric state, and a wider one rounds to an id the pool does not hold, so
    // every themed view silently draws the default theme.
    const uint64_t mixed = (state_ ^ (count_ * 0x9e3779b97f4a7c15ull)) & kMaxExactDouble;
    // Zero is the empty theme's id, so an empty digest would make two different
    // themes collide with "no theme" and with each other.
    return mixed == 0 ? 1 : mixed;
  }

private:
  uint64_t state_{0xcbf29ce484222325ull};
  uint64_t count_{0};
};

/// A color as ARGB bits. Android's `processColor` answers with a signed 32-bit
/// int and iOS's with an unsigned one; a negative double cast straight to
/// uint32_t is undefined and comes out as 0 on arm64, a transparent color that
/// hides every themed run. Through int64 both signs keep their bits.
uint32_t argbOf(double value) { return static_cast<uint32_t>(static_cast<int64_t>(value)); }

uint64_t bitsOf(double value) {
  uint64_t bits = 0;
  static_assert(sizeof(bits) == sizeof(value));
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}

/// Hashes one override, and returns how many entries it consumed: a color whose
/// dark half follows it is one value in two entries.
///
/// The pair is folded in as one unit rather than entry by entry, so a theme that
/// happens to set a slot and the id after it cannot hash the same as a real
/// pair of that color.
size_t hashOverride(Hasher &id, Hasher &layoutKey, const std::vector<uint32_t> &slots,
                    const std::vector<double> &values, size_t index) {
  const uint32_t slot = slots[index];
  // No color reaches `layoutKey`, because no color can change a size. Every
  // metric, weight and font slot does, or a theme could change a size without
  // the measurement noticing.
  const bool sizes = kSlotLayout[slot - 1] != 0;
  id.add(slot);
  if (sizes) {
    layoutKey.add(slot);
  }
  if (kSlotKinds[slot - 1] == static_cast<uint8_t>(SlotKind::Color)) {
    id.add(argbOf(values[index]));
    if (kSlotDark[slot - 1] != 0 && index + 1 < slots.size() && slots[index + 1] == kSlotDark[slot - 1]) {
      id.add(slots[index + 1]);
      id.add(argbOf(values[index + 1]));
      return 2;
    }
    // One half on its own, light or dark: the slot id already says which.
    return 1;
  }
  id.add(bitsOf(values[index]));
  if (sizes) {
    layoutKey.add(bitsOf(values[index]));
  }
  return 1;
}

/// The overrides a call is allowed to apply, in the order they arrive.
///
/// A malformed list is not a value a caller can send on purpose, so the whole
/// call falls back to the default theme: half a theme is worse than none, and a
/// caller that gets the default can see its value was refused.
std::vector<size_t> acceptedSlots(const std::vector<uint32_t> &slots, const std::vector<double> &values) {
  std::vector<size_t> accepted;
  if (slots.size() != values.size()) {
    return accepted;
  }
  uint32_t previous = 0;
  for (size_t index = 0; index < slots.size(); index++) {
    const uint32_t slot = slots[index];
    // Zero is not a slot, and anything at or past the count is an id from a
    // newer table than this build has.
    if (slot == 0 || slot >= kSlotIdCount) {
      continue;
    }
    if (slot <= previous) {
      accepted.clear();
      return accepted;
    }
    previous = slot;
    accepted.push_back(index);
  }
  return accepted;
}

struct Pool {
  std::mutex mutex;
  /// Every interned theme by id. Held weakly, so a theme a view still uses is
  /// kept alive by that view rather than by the pool.
  std::unordered_map<uint64_t, std::weak_ptr<const Theme>> byId;
  /// Insertion order, so the oldest can be dropped without a scan.
  std::list<uint64_t> order;
};

Pool &pool() {
  static Pool instance;
  return instance;
}

/// Drops themes nothing references and keeps the pool bounded. The lock is held.
void trim(Pool &state) {
  while (state.order.size() > kRetainedThemes) {
    const uint64_t oldest = state.order.front();
    state.order.pop_front();
    const auto entry = state.byId.find(oldest);
    if (entry != state.byId.end() && entry->second.expired()) {
      state.byId.erase(entry);
    }
  }
}

} // namespace

Theme::Handle Theme::create(const std::vector<uint32_t> &slots,
                             const std::vector<double> &values,
                             const std::vector<std::string> &fonts,
                             uint64_t id,
                             uint64_t layoutKey) {
  auto theme = std::shared_ptr<Theme>(new Theme());
  // Slot arrays use id - 1; kSlotIdCount is one past the highest valid id.
  const size_t count = kSlotIdCount - 1;
  theme->numbers_.resize(count);
  theme->colors_.resize(count);
  theme->fonts_.assign(count, -1);
  theme->numberMask_.assign(count, 0);

  for (size_t index = 0; index < count; index++) {
    if (kSlotKinds[index] == static_cast<uint8_t>(SlotKind::Color)) {
      const uint16_t role = kSlotColorDefaults[index];
      theme->colors_[index].defaultRole = role;
      if (role >= kRoleTokenClassBase) {
        // A syntax slot's default is a concrete color rather than a platform
        // role, so it is filled here, once, and both renderers draw an unset
        // class from the same palette rather than from nothing.
        const auto &palette = kTokenColors[role - kRoleTokenClassBase];
        theme->colors_[index].light = palette.light;
        theme->colors_[index].dark = palette.dark;
      }
    } else if (kSlotKinds[index] != static_cast<uint8_t>(SlotKind::Font)) {
      theme->numbers_[index] = kSlotDefaults[index];
    }
  }

  // Ids arrive sorted and unique from `src/theme.ts`, so one pass over them
  // applies a color pair as one unit: the light id, then the dark one.
  for (size_t index = 0; index < slots.size(); index++) {
    const uint32_t slot = slots[index];
    // Zero is not a slot and anything at or past the count is an id from a newer
    // table than this build has; either is skipped so the rest still applies.
    if (slot == 0 || slot >= kSlotIdCount || index >= values.size()) {
      continue;
    }
    const double value = values[index];
    switch (kSlotKinds[slot - 1]) {
      case static_cast<uint8_t>(SlotKind::Font): {
        const int64_t family = static_cast<int64_t>(value);
        if (family < 0 || static_cast<size_t>(family) >= fonts.size()) {
          break;
        }
        // Interned, so two slots naming one family share a string and a font
        // cache keyed by family cannot miss a hit.
        const std::string &name = fonts[family];
        const auto found = std::find(theme->fontNames_.begin(), theme->fontNames_.end(), name);
        if (found != theme->fontNames_.end()) {
          theme->fonts_[slot - 1] = static_cast<int32_t>(found - theme->fontNames_.begin());
          break;
        }
        theme->fontNames_.push_back(name);
        theme->fonts_[slot - 1] = static_cast<int32_t>(theme->fontNames_.size() - 1);
        theme->numberMask_[slot - 1] |= Theme::kSetMask;
        break;
      }
      case static_cast<uint8_t>(SlotKind::Color): {
        // Each half arrives on its own id, and only the halves the app named
        // arrive: a plain color sends both, `{ light }` the light id alone and
        // `{ dark }` the dark id alone. The resolved value lives on the light id
        // either way, so reading the dark id returns the same entry.
        const uint32_t own = kSlotLight[slot - 1];
        const uint32_t lightId = own == 0 ? slot : own;
        Theme::Color &color = theme->colors_[lightId - 1];
        color.set = true;
        if (own != 0) {
          color.dark = argbOf(value);
          color.darkSet = true;
          break;
        }
        color.light = argbOf(value);
        color.lightSet = true;
        const bool hasDark = index + 1 < slots.size() && slots[index + 1] == kSlotDark[slot - 1] &&
            index + 1 < values.size();
        if (hasDark) {
          color.dark = argbOf(values[index + 1]);
          color.darkSet = true;
          index++;
        }
        break;
      }
      default:
        theme->numbers_[slot - 1] = value;
        theme->numberMask_[slot - 1] |= Theme::kSetMask;
        break;
    }
  }

  theme->id_ = id;
  theme->layoutKey_ = layoutKey;
  return theme;
}

const Theme::Handle &Theme::defaultTheme() {
  // Built through the same path as any other theme, so the empty theme's
  // defaults come from the one generated table rather than a second list.
  static const Handle instance = [] {
    // Zero rather than a hash of nothing, so a cache key that holds the empty
    // theme's id is the key the same layout had before theming existed, and a
    // real theme can never collide with it.
    return create({}, {}, {}, 0, 0);
  }();
  return instance;
}

Theme::Handle ThemeRegistry::intern(const std::vector<uint32_t> &slots,
                                    const std::vector<double> &values,
                                    const std::vector<std::string> &fonts) {
  // An empty list is the empty theme, which is a static, so nothing is hashed
  // and no pool entry is made: "no theme" costs nothing on any thread.
  if (slots.empty()) {
    return Theme::defaultTheme();
  }
  const std::vector<size_t> accepted = acceptedSlots(slots, values);
  if (accepted.empty()) {
    // A malformed list, a length mismatch, or a theme whose every id is from a
    // newer slot table. Half a theme is worse than none, and a caller that gets
    // the default can see its value was refused.
    return Theme::defaultTheme();
  }

  Hasher idHash;
  Hasher layoutHash;
  idHash.add(kHashVersion);
  layoutHash.add(kHashVersion);
  bool hasFonts = false;
  bool hasLayout = false;
  for (size_t index = 0; index < accepted.size();) {
    hasFonts |= kSlotKinds[slots[accepted[index]] - 1] == static_cast<uint8_t>(SlotKind::Font);
    hasLayout |= kSlotLayout[slots[accepted[index]] - 1] != 0;
    index += hashOverride(idHash, layoutHash, slots, values, accepted[index]);
  }
  // Font values are indices, so their array's ordered names are part of both
  // identities. Lengths separate names and preserve numeric array addressing.
  // Arrays without a font override have no effect on rendering or layout.
  if (hasFonts) {
    idHash.add(kSlotIdCount);
    layoutHash.add(kSlotIdCount);
    idHash.add(fonts.size());
    layoutHash.add(fonts.size());
    for (const std::string &font : fonts) {
      idHash.add(font);
      layoutHash.add(font);
    }
  }
  const uint64_t id = idHash.digest();
  // Colors alone retain the default metrics, including when a view first
  // switches from the default theme to a palette.
  const uint64_t layoutKey = hasLayout ? layoutHash.digest() : 0;

  Pool &state = pool();
  std::lock_guard<std::mutex> lock(state.mutex);
  const auto found = state.byId.find(id);
  if (found != state.byId.end()) {
    // The ids agree, so the content does too: two themes that hash the same
    // over the same slots with the same values describe the same drawing, and
    // rebuilding one to compare it would cost more than the collision could.
    if (auto existing = found->second.lock()) {
      state.order.remove(id);
      state.order.push_back(id);
      return existing;
    }
    state.byId.erase(found);
    state.order.remove(id);
  }

  Theme::Handle handle = Theme::create(slots, values, fonts, id, layoutKey);
  state.byId[id] = handle;
  state.order.push_back(id);
  trim(state);
  return handle;
}

Theme::Handle ThemeRegistry::find(uint64_t id) {
  // Zero is the default theme's own id, and it is a static rather than a pool
  // entry, so it is answered without taking the lock at all.
  if (id == 0) {
    return Theme::defaultTheme();
  }
  Pool &state = pool();
  std::lock_guard<std::mutex> lock(state.mutex);
  const auto found = state.byId.find(id);
  if (found == state.byId.end()) {
    return nullptr;
  }
  // A weak pointer the theme has already gone out of. A caller that holds a
  // handle to the theme cannot get here, so this means the id came from
  // somewhere else and the answer is genuinely absent rather than expired.
  return found->second.lock();
}

void ThemeRegistry::clear() {
  Pool &state = pool();
  std::lock_guard<std::mutex> lock(state.mutex);
  state.byId.clear();
  state.order.clear();
}

} // namespace downstream
