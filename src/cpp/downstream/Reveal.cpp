#include "Reveal.h"

#include "Caret.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace downstream {
namespace {

/// Whitespace as CommonMark defines it. A unit ends after it rather than inside
/// it, so a clip edge or a glyph split never falls between a word and the space
/// that follows it.
bool isSpace(char16_t unit) {
  return unit == u' ' || unit == u'\t' || unit == u'\n' || unit == u'\r' || unit == 0x0B || unit == 0x0C;
}

constexpr char16_t kZeroWidthJoiner = 0x200D;

/// The Unicode scalar at `index`, with how many UTF-16 units it occupies.
///
/// Several of the ranges that matter here — the CJK extensions, the emoji, the
/// variation selectors — live above the BMP and so arrive as a surrogate pair.
/// Classifying code units instead would call every one of them out of range.
uint32_t scalarAt(std::u16string_view text, size_t index, size_t &width) {
  const char16_t unit = text[index];
  if (unit >= 0xD800 && unit <= 0xDBFF && index + 1 < text.size() && text[index + 1] >= 0xDC00 &&
      text[index + 1] <= 0xDFFF) {
    width = 2;
    return 0x10000 + ((static_cast<uint32_t>(unit) - 0xD800) << 10) + (static_cast<uint32_t>(text[index + 1]) - 0xDC00);
  }
  width = 1;
  return unit;
}

/// Ideographs, kana, Hangul syllables, and the punctuation that belongs with
/// them. A paragraph of these has no spaces, so a whitespace split would fade
/// the whole thing as one block; one character each is what makes a CJK stream
/// cascade at all.
bool isIdeographic(uint32_t scalar) {
  return (scalar >= 0x1100 && scalar <= 0x11FF) || (scalar >= 0x2E80 && scalar <= 0x303F) ||
      (scalar >= 0x3040 && scalar <= 0x30FF) || (scalar >= 0x3130 && scalar <= 0x318F) ||
      (scalar >= 0x31C0 && scalar <= 0x31FF) || (scalar >= 0x3200 && scalar <= 0x33FF) ||
      (scalar >= 0x3400 && scalar <= 0x4DBF) || (scalar >= 0x4E00 && scalar <= 0x9FFF) ||
      (scalar >= 0xA960 && scalar <= 0xA97F) || (scalar >= 0xAC00 && scalar <= 0xD7A3) ||
      (scalar >= 0xF900 && scalar <= 0xFAFF) || (scalar >= 0xFE30 && scalar <= 0xFE4F) ||
      (scalar >= 0xFF00 && scalar <= 0xFF60) || (scalar >= 0x20000 && scalar <= 0x3FFFD);
}

bool isCombining(uint32_t scalar) {
  return (scalar >= 0x0300 && scalar <= 0x036F) || (scalar >= 0x1AB0 && scalar <= 0x1AFF) ||
      (scalar >= 0x1DC0 && scalar <= 0x1DFF) || (scalar >= 0x20D0 && scalar <= 0x20FF) ||
      (scalar >= 0xFE20 && scalar <= 0xFE2F);
}

bool isVariationSelector(uint32_t scalar) {
  return (scalar >= 0xFE00 && scalar <= 0xFE0F) || (scalar >= 0xE0100 && scalar <= 0xE01EF);
}

bool isRegionalIndicator(uint32_t scalar) { return scalar >= 0x1F1E6 && scalar <= 0x1F1FF; }

/// True when the two texts agree at `a`/`b` for as many of the next three
/// characters as both of them have.
///
/// One character alone confirms too easily: a stream of `**` or `[]` has plenty
/// of places where the next letter happens to match. Three is what separates "the
/// writer closed a span" from "the next word starts with this letter".
bool confirmsRun(std::u16string_view a, size_t ai, std::u16string_view b, size_t bi) {
  const size_t available = std::min(a.size() - ai, b.size() - bi);
  if (available == 0) {
    return false;
  }
  const size_t wanted = std::min<size_t>(3, available);
  for (size_t k = 0; k < wanted; k++) {
    if (a[ai + k] != b[bi + k]) {
      return false;
    }
  }
  return true;
}

/// How far one char-mode unit reaches: a surrogate pair, a base character with
/// its combining marks and variation selectors, a zero-width-joiner sequence, or
/// a pair of regional indicators.
///
/// This is approximate segmentation rather than full UAX #29, which needs tables
/// this library does not carry. The cases that matter for a fade — an emoji that
/// must not be split across two alphas, an accented letter that must not lose its
/// mark — are all covered, and the tests pin the common ones.
size_t clusterEnd(std::u16string_view text, size_t from, size_t to) {
  size_t i = from;
  size_t width = 1;
  uint32_t scalar = scalarAt(text, i, width);
  i += width;
  if (isRegionalIndicator(scalar) && i < to) {
    size_t next = 1;
    if (isRegionalIndicator(scalarAt(text, i, next))) {
      return i + next;
    }
  }
  while (i < to) {
    scalar = scalarAt(text, i, width);
    if (isCombining(scalar) || isVariationSelector(scalar)) {
      i += width;
      continue;
    }
    if (text[i] == kZeroWidthJoiner && i + 1 < to) {
      i++;
      scalar = scalarAt(text, i, width);
      i += width;
      continue;
    }
    break;
  }
  return i;
}

/// Pushes a new range's unit, unless it is whitespace, which belongs to the
/// unit before it.
void pushUnit(std::vector<RevealUnit> &out, uint32_t blockIndex, uint32_t from, uint32_t to) {
  if (from >= to) {
    return;
  }
  out.push_back({blockIndex, from, to, 0});
}

} // namespace

float CubicBezier::solve(float t) const {
  if (t <= 0 || t >= 1) {
    return t <= 0 ? 0.0f : 1.0f;
  }
  // The x of the curve at parameter u, and its derivative.
  const auto sampleX = [this](float u) {
    const float v = 1 - u;
    return 3 * v * v * u * x1 + 3 * v * u * u * x2 + u * u * u;
  };
  const auto sampleDX = [this](float u) {
    const float v = 1 - u;
    return 3 * v * v * x1 + 6 * v * u * (x2 - x1) + 3 * u * u * (1 - x2);
  };
  // Newton converges in a couple of steps near the answer and costs a division
  // each time; bisection is the fallback where the derivative is too small to be
  // useful, which is exactly where a curve is flat.
  float u = t;
  for (int step = 0; step < 8; step++) {
    const float x = sampleX(u) - t;
    if (std::fabs(x) < 1e-6f) {
      break;
    }
    const float dx = sampleDX(u);
    if (std::fabs(dx) < 1e-6f) {
      break;
    }
    u -= x / dx;
  }
  if (u < 0 || u > 1) {
    float low = 0;
    float high = 1;
    u = t;
    for (int step = 0; step < 32; step++) {
      const float x = sampleX(u);
      if (std::fabs(x - t) < 1e-6f) {
        break;
      }
      if (x < t) {
        low = u;
      } else {
        high = u;
      }
      u = (low + high) / 2;
    }
  }
  const float v = 1 - u;
  return 3 * v * v * u * y1 + 3 * v * u * u * y2 + u * u * u;
}

std::vector<AlignedRun> alignVisibleText(std::u16string_view prev, std::u16string_view next) {
  std::vector<AlignedRun> runs;
  if (prev.empty() || next.empty()) {
    return runs;
  }
  if (prev == next) {
    // The common case by far: an append-only block that has not changed. A
    // memcmp answers it without walking a character.
    runs.push_back({0, 0, static_cast<uint32_t>(next.size())});
    return runs;
  }
  if (next.size() >= prev.size() && next.substr(0, prev.size()) == prev) {
    // Appends dominate streaming. Compare the old prefix in one contiguous
    // operation and only segment the newly arrived suffix.
    runs.push_back({0, 0, static_cast<uint32_t>(prev.size())});
    return runs;
  }
  if (prev.size() > kRevealAlignLimit || next.size() > kRevealAlignLimit) {
    // Streamdown's rule, unchanged: below the old length is what the reader has
    // seen. Alignment on a block this size costs more than the occasional
    // re-fade is worth.
    runs.push_back({0, 0, static_cast<uint32_t>(std::min(prev.size(), next.size()))});
    return runs;
  }

  constexpr size_t kSkipBudget = 4;
  size_t pi = 0;
  size_t ni = 0;
  size_t runStartNext = 0;
  size_t runStartPrev = 0;
  bool open = false;
  const auto flush = [&] {
    if (open && ni > runStartNext) {
      runs.push_back({static_cast<uint32_t>(runStartNext), static_cast<uint32_t>(runStartPrev),
                      static_cast<uint32_t>(ni - runStartNext)});
    }
    open = false;
  };

  while (pi < prev.size() && ni < next.size()) {
    if (prev[pi] == next[ni]) {
      if (!open) {
        open = true;
        runStartNext = ni;
        runStartPrev = pi;
      }
      pi++;
      ni++;
      continue;
    }
    // The characters differ. A closing `**`, a closing backtick, or the `](` of
    // a link all make the old text hold something the new one does not, so one
    // side usually steps over it and the walk resumes.
    flush();
    bool moved = false;
    for (size_t skip = 1; skip <= kSkipBudget && pi + skip < prev.size(); skip++) {
      if (confirmsRun(prev, pi + skip, next, ni)) {
        pi += skip;
        moved = true;
        break;
      }
    }
    if (!moved) {
      for (size_t skip = 1; skip <= kSkipBudget && ni + skip < next.size(); skip++) {
        if (confirmsRun(next, ni + skip, prev, pi)) {
          ni += skip;
          moved = true;
          break;
        }
      }
    }
    if (!moved) {
      // Nothing lines up any more, so the rest of the new text is new. Coming
      // back would only find more of the same.
      break;
    }
  }
  flush();
  return runs;
}

void splitUnits(const Block &block, uint32_t blockIndex, uint32_t from, uint32_t to, RevealSplit split,
                std::vector<RevealUnit> &out) {
  // A block with no words of its own is one unit. A rule and an empty fence draw
  // through a single zero-width placeholder, and a table row's cells are laid
  // out by the grid rather than by a text run, so neither can be split into
  // words however the reader's `sep` reads. The range is empty in that case, so
  // this is checked before the range is.
  if (block.kind == BlockKind::ThematicBreak || block.text.empty()) {
    pushUnit(out, blockIndex, 0, 1);
    return;
  }
  if (from >= to || to > block.text.size()) {
    return;
  }
  if (block.kind == BlockKind::TableRow) {
    pushUnit(out, blockIndex, from, to);
    return;
  }

  const std::u16string_view text(block.text);
  if (split == RevealSplit::Char) {
    size_t i = from;
    while (i < to) {
      if (isSpace(text[i])) {
        // Whitespace rides on the unit before it, as in Streamdown's
        // `splitByChar`: a space on its own is not something that fades in.
        if (!out.empty() && out.back().block == blockIndex && out.back().end == i) {
          size_t end = i;
          while (end < to && isSpace(text[end])) {
            end++;
          }
          out.back().end = static_cast<uint32_t>(end);
          i = end;
          continue;
        }
        while (i < to && isSpace(text[i])) {
          i++;
        }
        continue;
      }
      const size_t end = clusterEnd(text, i, to);
      pushUnit(out, blockIndex, static_cast<uint32_t>(i), static_cast<uint32_t>(end));
      i = end;
    }
    return;
  }

  size_t i = from;
  while (i < to) {
    size_t width = 1;
    const uint32_t scalar = scalarAt(text, i, width);
    // A line break ends a unit: the characters after it are a new line, and
    // splitting glyphs across a paragraph boundary is not something a fade has
    // any business doing.
    if (isSpace(text[i])) {
      i += width;
      continue;
    }
    if (isIdeographic(scalar)) {
      pushUnit(out, blockIndex, static_cast<uint32_t>(i), static_cast<uint32_t>(i + width));
      i += width;
      continue;
    }
    const size_t start = i;
    while (i < to) {
      if (isSpace(text[i])) {
        break;
      }
      size_t next = 1;
      if (isIdeographic(scalarAt(text, i, next))) {
        break;
      }
      i += next;
    }
    // Trailing spaces belong to the word they follow, so an underline under a
    // link does not appear a moment before its text.
    while (i < to && isSpace(text[i]) && text[i] != u'\n') {
      i++;
    }
    pushUnit(out, blockIndex, static_cast<uint32_t>(start), static_cast<uint32_t>(i));
  }
}

namespace {

/// Carries one block's settled ranges forward through an alignment. Ranges are
/// half-open in `next`'s coordinates once this returns, and one that no longer
/// covers any character is dropped.
std::vector<AlignedRun> mapForward(const std::vector<AlignedRun> &settled,
                                   const std::vector<AlignedRun> &runs) {
  std::vector<AlignedRun> mapped;
  size_t first = 0;
  for (const AlignedRun &range : settled) {
    const uint32_t end = range.nextStart + range.length;
    while (first < runs.size() && runs[first].prevStart + runs[first].length <= range.nextStart) {
      ++first;
    }
    for (size_t index = first; index < runs.size() && runs[index].prevStart < end; ++index) {
      const AlignedRun &run = runs[index];
      const uint32_t from = std::max(range.nextStart, run.prevStart);
      const uint32_t to = std::min(end, run.prevStart + run.length);
      if (from < to) {
        const uint32_t at = run.nextStart + (from - run.prevStart);
        mapped.push_back({at, at, to - from});
      }
    }
  }
  return mapped;
}

/// Adds `[start, end)` to a block's sorted, non-overlapping ranges. Nothing is
/// dropped: settled text only ever grows, because a character that has been
/// drawn stays drawn.
void addRange(std::vector<AlignedRun> &ranges, uint32_t start, uint32_t end) {
  if (start >= end) {
    return;
  }
  // Snapshots add ranges in text order. Most additions extend
  // the last range, so do not sort the whole list for every word.
  if (!ranges.empty() && start >= ranges.back().nextStart) {
    AlignedRun &last = ranges.back();
    if (start <= last.nextStart + last.length) {
      last.length = std::max(end, last.nextStart + last.length) - last.nextStart;
    } else {
      ranges.push_back({start, 0, end - start});
    }
    return;
  }
  auto first = std::lower_bound(ranges.begin(), ranges.end(), start,
                               [](const AlignedRun &range, uint32_t at) {
                                 return range.nextStart + range.length < at;
                               });
  auto last = first;
  while (last != ranges.end() && last->nextStart <= end) {
    start = std::min(start, last->nextStart);
    end = std::max(end, last->nextStart + last->length);
    ++last;
  }
  const auto at = ranges.erase(first, last);
  ranges.insert(at, {start, 0, end - start});
}

/// How much of `[start, end)` this block's settled ranges already cover.
uint32_t overlapWith(const std::vector<AlignedRun> &ranges, uint32_t start, uint32_t end) {
  uint32_t covered = 0;
  for (const AlignedRun &range : ranges) {
    if (range.nextStart >= end) break;
    const uint32_t from = std::max(start, range.nextStart);
    const uint32_t to = std::min(end, range.nextStart + range.length);
    if (from < to) {
      covered += to - from;
    }
  }
  return covered;
}

double revealBacklog(const RevealOptions &options) {
  if (options.staggerMs <= 0 || options.durationMs <= 0) return 0;
  return std::max(0.0, static_cast<double>(options.maxBacklogMs));
}

// Existing future units can outlive a smaller budget, for example after Android
// reduces its animator scale. Move them earlier in the same temporal order.
// Started fades retain their timestamps and never lose progress.
void compressFutureStarts(std::span<RevealUnit> units, double now, const RevealOptions &options) {
  const double backlog = revealBacklog(options);
  const double deadline = now + backlog;
  double latest = now;
  for (const RevealUnit &unit : units) latest = std::max(latest, unit.startAt);
  if (latest <= deadline) return;
  const double scale = backlog / (latest - now);
  for (RevealUnit &unit : units) {
    if (unit.startAt > now) {
      unit.startAt = std::min(deadline, now + (unit.startAt - now) * scale);
    }
  }
}

} // namespace

void RevealTimeline::schedule(std::span<RevealUnit> units, double now, const RevealOptions &options) {
  if (units.empty()) {
    return;
  }
  const double stagger = options.durationMs <= 0 ? 0 : std::max(0.0, static_cast<double>(options.staggerMs));
  const double backlog = revealBacklog(options);
  const double budgetEnd = now + backlog;
  const double startAt = stagger == 0 ? now : std::clamp(committedNextStartAt_, now, budgetEnd);
  // The budget wins over the requested spacing. An exhausted pass starts its
  // remaining units together rather than accumulating invisible text debt.
  const double step = units.size() > 1
      ? std::min(stagger, (budgetEnd - startAt) / static_cast<double>(units.size() - 1)) : stagger;
  for (size_t index = 0; index < units.size(); index++) {
    units[index].startAt = std::min(budgetEnd, startAt + static_cast<double>(index) * step);
  }
  committedNextStartAt_ = std::min(budgetEnd, startAt + static_cast<double>(units.size()) * step);
}

void RevealTracker::seed(DocumentHandle shown) {
  // Seeding is a statement about what is on screen, not a reset: the units
  // already fading keep their timing, and only the "already shown" document
  // changes. A view that rebinds mid-stream therefore does not restart the
  // fades it was already showing.
  shown_ = std::move(shown);
}

const std::vector<RevealUnit> &RevealTracker::apply(const DocumentHandle &next, double now, const RevealOptions &options) {
  if (!next) {
    clear();
    return active_;
  }
  if (options.effect == RevealEffect::None) {
    // The fade is off, which is either an explicit request or Reduce Motion.
    // Every unit finishes at once and nothing is scheduled from here, and the
    // document is still recorded as shown so turning the fade back on does not
    // reveal text that has been on screen all along.
    shown_ = next;
    active_.clear();
    settled_.clear();
    timeline_.clear();
    return active_;
  }

  compressFutureStarts(std::span<RevealUnit>(active_), now, options);
  if (next == shown_) {
    return active_;
  }

  static const Document emptyShown;
  const Document &previousDocument = shown_ ? *shown_ : emptyShown;
  const size_t count = next->blocks.size();
  const size_t previousCount = previousDocument.blocks.size();
  const size_t shared = std::min(previousDocument.blocks.sharedSize(), next->blocks.sharedSize());
  // Prefix::extend only appends immutable chunks. The same last block proves
  // that these snapshots share the entire preceding prefix. Equal prefix sizes
  // alone do not prove this after an edit or a stream reset.
  const size_t unchangedPrefix = shared != 0 &&
      &previousDocument.blocks[shared - 1] == &next->blocks[shared - 1] ? shared : 0;

  settled_.resize(count);
  std::vector<RevealUnit> carried;
  carried.reserve(active_.size());
  size_t activeIndex = 0;
  while (activeIndex < active_.size() && active_[activeIndex].block < unchangedPrefix) {
    carried.push_back(active_[activeIndex++]);
  }
  std::vector<RevealUnit> fresh;
  for (size_t index = unchangedPrefix; index < count; ++index) {
    const Block &block = next->blocks[index];
    const Block *previous = index < previousCount ? &previousDocument.blocks[index] : nullptr;
    const bool unchanged = previous != nullptr &&
        ((block.identity != 0 && block.identity == previous->identity) || previous->text == block.text);
    const size_t firstUnit = activeIndex;
    while (activeIndex < active_.size() && active_[activeIndex].block == index) ++activeIndex;
    if (unchanged) {
      carried.insert(carried.end(), active_.begin() + firstUnit, active_.begin() + activeIndex);
      continue;
    }

    const auto runs = previous != nullptr ? alignVisibleText(previous->text, block.text) : std::vector<AlignedRun>{};
    settled_[index] = mapForward(settled_[index], runs);
    size_t firstRun = 0;
    for (size_t unitIndex = firstUnit; unitIndex < activeIndex; ++unitIndex) {
      const RevealUnit &unit = active_[unitIndex];
      while (firstRun < runs.size() && runs[firstRun].prevStart + runs[firstRun].length <= unit.start) ++firstRun;
      for (size_t runIndex = firstRun; runIndex < runs.size() && runs[runIndex].prevStart < unit.end; ++runIndex) {
        const AlignedRun &run = runs[runIndex];
        const uint32_t from = std::max(unit.start, run.prevStart);
        const uint32_t to = std::min(unit.end, run.prevStart + run.length);
        if (from < to) {
          const uint32_t at = run.nextStart + (from - run.prevStart);
          carried.push_back({unit.block, at, at + (to - from), unit.startAt});
        }
      }
    }

    const uint32_t length = static_cast<uint32_t>(block.text.size());
    uint32_t cursor = 0;
    for (const AlignedRun &run : runs) {
      if (run.nextStart > cursor) splitUnits(block, static_cast<uint32_t>(index), cursor, run.nextStart, options.split, fresh);
      cursor = std::max(cursor, run.nextStart + run.length);
    }
    if (cursor < length || (previous == nullptr && length == 0)) {
      splitUnits(block, static_cast<uint32_t>(index), cursor, length, options.split, fresh);
    }
  }

  // The invariant, checked where the alignment is. A unit that covers a character
  // the reader has already seen drawn at full alpha is a re-reveal: it would make
  // text a reader has read blink out and back. Closing a `**` is not that — the
  // markers disappear and the word they styled is genuinely new text in a
  // different place — and the alignment is what tells the two apart.
  for (const RevealUnit &unit : fresh) {
    reReveals_ += overlapWith(settled_[unit.block], unit.start, unit.end);
  }

  // A word the writer split across two snapshots (`wor`, then `world`) has a new
  // range that butts against a unit still fading with nothing between them. It
  // is the same word, so it joins that unit and keeps its start time rather than
  // restarting a cascade on a half word. A part that has already finished is no
  // longer in `carried`, so the rest gets a unit of its own.
  std::vector<RevealUnit> scheduled;
  scheduled.reserve(fresh.size());
  size_t joinIndex = 0;
  for (RevealUnit &unit : fresh) {
    const Block &block = next->blocks[unit.block];
    while (joinIndex < carried.size() &&
           (carried[joinIndex].block < unit.block ||
            (carried[joinIndex].block == unit.block && carried[joinIndex].end < unit.start))) {
      ++joinIndex;
    }
    bool joined = joinIndex < carried.size() && carried[joinIndex].block == unit.block &&
        carried[joinIndex].end == unit.start && unit.start != 0 && !isSpace(block.text[unit.start - 1]);
    if (joined) carried[joinIndex].end = unit.end;
    if (!joined) {
      scheduled.push_back(unit);
    }
  }

  timeline_.schedule(std::span<RevealUnit>(scheduled), now, options);

  // Both lists are already in block/text order. Merge them once rather than
  // sorting every active character again after each streamed append.
  active_.clear();
  active_.reserve(carried.size() + scheduled.size());
  std::merge(carried.begin(), carried.end(), scheduled.begin(), scheduled.end(), std::back_inserter(active_),
             [](const RevealUnit &a, const RevealUnit &b) {
               return a.block != b.block ? a.block < b.block : a.start < b.start;
             });
  // Carried ranges are already recorded through the alignment. Only newly
  // scheduled text extends the record used by the next invariant check.
  for (const RevealUnit &unit : fresh) {
    addRange(settled_[unit.block], unit.start, unit.end);
  }
  shown_ = next;
  return active_;
}

bool RevealTracker::prune(double now, const RevealOptions &options) {
  const auto finished = [&](const RevealUnit &unit) {
    return now - unit.startAt >= static_cast<double>(options.durationMs);
  };
  const auto last = std::remove_if(active_.begin(), active_.end(), finished);
  active_.erase(last, active_.end());
  return !active_.empty();
}

void RevealTracker::clear() {
  active_.clear();
  settled_.clear();
  timeline_.clear();
  shown_.reset();
}

std::span<const RevealUnit> revealUnitsForBlock(std::span<const RevealUnit> units, uint32_t block) {
  if (units.empty()) return {};
  const auto first = std::lower_bound(units.begin(), units.end(), block,
                                     [](const RevealUnit &unit, uint32_t index) { return unit.block < index; });
  const auto last = std::upper_bound(first, units.end(), block,
                                    [](uint32_t index, const RevealUnit &unit) { return index < unit.block; });
  return units.subspan(static_cast<size_t>(first - units.begin()), static_cast<size_t>(last - first));
}

float revealAlpha(const RevealUnit &unit, double now, const RevealOptions &options) {
  if (options.effect == RevealEffect::None) {
    return 1.0f;
  }
  const double duration = static_cast<double>(options.durationMs);
  // A fade with no duration is no fade, and the platform's own animator scale can
  // ask for one, so the text appears at once rather than never.
  if (duration <= 0) {
    return 1.0f;
  }
  const double elapsed = now - unit.startAt;
  if (elapsed <= 0) {
    return 0.0f;
  }
  if (elapsed >= duration) {
    return 1.0f;
  }
  return options.easing.solve(static_cast<float>(elapsed / duration));
}

float revealRise(const RevealUnit &unit, double now, const RevealOptions &options) {
  if (options.effect != RevealEffect::Rise) {
    return 0;
  }
  const double duration = static_cast<double>(options.durationMs);
  if (duration <= 0) {
    return 0;
  }
  const double elapsed = now - unit.startAt;
  if (elapsed <= 0) {
    return kRevealRisePoints;
  }
  if (elapsed >= duration) {
    return 0;
  }
  const float progress = options.easing.solve(static_cast<float>(elapsed / duration));
  return kRevealRisePoints * (1.0f - progress);
}

} // namespace downstream
