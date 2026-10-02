#pragma once

// Which text is new, when each piece of it starts, and how far through its fade
// it is at a given moment.
//
// Streamdown runs this in the browser with a rehype plugin that wraps every word
// in a span and lets CSS keyframes run. Natively there is no CSS and no span per
// word, so the decisions move here instead: the engine decides what is new and
// when each unit starts, and each platform's existing draw pass reads the alpha
// for the characters it is about to draw. Everything in this header is a pure
// function or a small value type, so all of it is host-testable without a
// device, and a frame of animation costs no parse and no relayout.

#include "Document.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace downstream {

/// Soft budget for how far ahead of wall-clock a cascade may be scheduled.
/// Fast streams compress their stagger to fit rather than queueing an unbounded
/// run of invisible words. Ported from Streamdown's
/// `MAX_ANIMATION_BACKLOG_MS`.
constexpr double kRevealMaxBacklogMs = 320.0;

/// Floor for the per-unit stagger under compression, so a multi-block pass does
/// not collapse to a single step once earlier blocks have spent the budget.
/// Ported from Streamdown's `MIN_STAGGER_STEP_MS`.
constexpr double kRevealMinStaggerStepMs = 4.0;

/// How far a `Rise` unit travels as it appears, in points. A placeholder, a
/// marker and a table row all travel the same distance so a document's parts
/// move together, and it is small enough that a line's height never changes.
constexpr float kRevealRisePoints = 4.0f;

/// A block over this size is judged by Streamdown's character-count rule rather
/// than by alignment. The alignment walk is linear in the worst case and a
/// 64 KiB block of prose is not one a reader is watching characters arrive in.
constexpr uint32_t kRevealAlignLimit = 64 * 1024;

enum class RevealSplit : uint8_t {
  /// A maximal run of non-whitespace, with the whitespace after it glued on.
  Word = 0,
  /// One grapheme-ish cluster at a time.
  Char = 1,
};

/// What a unit does as it appears. `blurIn` is not here: neither platform can
/// blur one glyph run cheaply in a text draw pass, so `src/animation.ts` maps it
/// to `Fade` and warns once.
enum class RevealEffect : uint8_t {
  /// No fade at all. Every text still parses; it simply arrives at full alpha.
  None = 0,
  Fade = 1,
  /// Alpha, plus a short rise of a few points.
  Rise = 2,
};

/// A CSS cubic-bezier easing curve, solved for y given x.
///
/// Written here rather than taken from a platform so one curve answers on both:
/// iOS has no public bezier and Android's `PathInterpolator` is API 21+ with its
/// own float precision, so the engine's own solver is what the golden values are
/// checked against.
struct CubicBezier {
  float x1{0.25f};
  float y1{0.1f};
  float x2{0.25f};
  float y2{1.0f};

  /// Progress for an input progress of `t` in [0, 1].
  float solve(float t) const;

  bool operator==(const CubicBezier &other) const = default;
};

/// Everything that decides when a character appears. Deliberately a value: it is
/// part of a cache key and crosses JNI as a flat array of doubles.
struct RevealOptions {
  RevealEffect effect{RevealEffect::None};
  RevealSplit split{RevealSplit::Word};
  float durationMs{150.0f};
  float staggerMs{40.0f};
  float maxBacklogMs{static_cast<float>(kRevealMaxBacklogMs)};
  /// CSS `ease`.
  CubicBezier easing{0.25f, 0.1f, 0.25f, 1.0f};

  bool operator==(const RevealOptions &other) const {
    return effect == other.effect && split == other.split && durationMs == other.durationMs &&
        staggerMs == other.staggerMs && maxBacklogMs == other.maxBacklogMs && easing == other.easing;
  }
};

/// A run of new visible text, in one block's text coordinates, with the time its
/// fade starts on the shared timeline.
///
/// `block` and `start`/`end` are what both platforms already map into their
/// segment strings, so a fade keeps its timing when its segment is rebuilt,
/// committed, or swapped for a cached one.
struct RevealUnit {
  uint32_t block{0};
  /// UTF-16, into `Block::text`.
  uint32_t start{0};
  uint32_t end{0};
  /// Milliseconds on the platform's monotonic clock.
  double startAt{0};
};

/// A stretch of `next` that matches the same stretch of `prev`. Everything in
/// `next` that no run covers is new.
struct AlignedRun {
  uint32_t nextStart{0};
  uint32_t prevStart{0};
  uint32_t length{0};
};

/// Aligns two versions of one block's visible text and reports which stretches
/// of the newer one were already on screen.
///
/// A two-pointer walk with a small skip budget, rather than Streamdown's plain
/// character count: when `**Note` becomes `Note` the closing word is new and
/// fades in, where a count would say it appeared two characters past the old
/// length and show it at once. A block over `kRevealAlignLimit` falls back to
/// the count rule.
std::vector<AlignedRun> alignVisibleText(std::u16string_view prev, std::u16string_view next);

/// Cuts the new range [from, to) of `block` into units.
///
/// Words take their trailing whitespace with them (Streamdown #535, so a link
/// underline never shows before its word), a CJK syllable or mark is a unit of
/// its own because a paragraph of it has no spaces, a line break ends a unit,
/// and a block with no words of its own — a rule, a table row, an empty fence —
/// is one unit.
void splitUnits(const Block &block, uint32_t blockIndex, uint32_t from, uint32_t to, RevealSplit split,
                std::vector<RevealUnit> &out);

/// A port of Streamdown's `createAnimateTimeline`: one cursor shared by every
/// block and every applied snapshot, so a burst compresses into the budget
/// rather than queueing seconds of invisible words.
class RevealTimeline {
public:
  /// Gives every unit its start time. One call is one pass: Streamdown's
  /// `beginPass`, `take` and `commitPass`, with no render to rewind for.
  void schedule(std::span<RevealUnit> units, double now, const RevealOptions &options);
  /// Forgets the cursor, for a new generation.
  void clear() { committedNextStartAt_ = 0; }
  double cursor() const { return committedNextStartAt_; }

private:
  double committedNextStartAt_{0};
};

/// Per-view bookkeeping for what is still fading.
///
/// It holds the document the view last showed, the timeline cursor, and the
/// units that are not yet at full alpha. None of that lives in a rendering: a
/// segment is shared and immutable, so alpha can only ever be applied in a draw
/// pass. The invariant the class exists to keep is that a character drawn at
/// full alpha is never scheduled again.
class RevealTracker {
public:
  /// Records what the view has already put on screen, so the next `apply` treats
  /// it as old.
  ///
  /// A fresh controlled view seeds the first document it shows, which is what
  /// makes a message that mounts complete appear without a fade. A view that
  /// binds to a live stream seeds the session's own record of what was shown, so
  /// a virtualized list scrolling a message back into view does not fade all of
  /// it again. Seeding nothing is the case that fades everything, which is a
  /// reset: the new source has shown nothing.
  void seed(DocumentHandle shown);

  /// Aligns `next` against the shown document, keeps the timing of the units
  /// whose characters still align, schedules the new ones, and returns the active
  /// set.
  ///
  /// With the fade off this still records the document as shown and schedules
  /// nothing, so turning the fade on later does not reveal a document that has
  /// been on screen all along.
  const std::vector<RevealUnit> &apply(const DocumentHandle &next, double now, const RevealOptions &options);

  /// Drops the units that have finished. False when nothing is left to draw,
  /// which is what stops the platform's ticker.
  ///
  /// A unit is kept until this runs rather than dropped the moment its fade
  /// ends, because the frame after that is the one that draws it at full alpha;
  /// dropping it early would leave the last frame at whatever alpha it had.
  bool prune(double now, const RevealOptions &options);

  /// Finishes every unit at once and forgets what was shown, for a view leaving
  /// its window, a recycled view, or a new generation. The next document fades
  /// from its first word.
  void clear();

  /// True while any unit still needs drawing, which is what keeps a platform's
  /// ticker running.
  bool active() const { return !active_.empty(); }
  /// The units still fading. A draw pass walks this on every frame, so it is the
  /// whole per-frame work: a lookup, never a re-align.
  const std::vector<RevealUnit> &units() const { return active_; }
  const DocumentHandle &shown() const { return shown_; }

  /// Characters this tracker has scheduled that an earlier `apply` had already
  /// reported as old. Always zero.
  ///
  /// The invariant is the whole reason this class exists: a character drawn at
  /// full alpha must never fade in again. It is checked here rather than by a
  /// caller because a character's identity across two snapshots is the
  /// alignment's answer, not its offsets' — closing a `**` legitimately reports
  /// the word it was styling as new text again, and no offset-based check can
  /// tell that from a real re-reveal.
  uint64_t reReveals() const { return reReveals_; }

private:
  /// Null when nothing has been shown yet, which is what makes every character
  /// of the next document new.
  DocumentHandle shown_;
  RevealTimeline timeline_;
  std::vector<RevealUnit> active_;
  /// What is at full alpha, per block, in `shown_`'s coordinates. Kept so an
  /// apply can prove the invariant above; a caller never reads it.
  std::vector<std::vector<AlignedRun>> settled_;
  uint64_t reReveals_{0};
};

/// A unit's alpha at `now`: 0 before it starts, eased progress during the fade,
/// 1 after. The fade never changes layout, so nothing else has to move.
float revealAlpha(const RevealUnit &unit, double now, const RevealOptions &options);

/// How far a `Rise` unit has to travel, in points, at `now`: four points at the
/// start of the fade and none at the end. Zero for every other effect.
float revealRise(const RevealUnit &unit, double now, const RevealOptions &options);

} // namespace downstream