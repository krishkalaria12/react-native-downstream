#pragma once

// Provisional styling for the unstable tail of a stream.
//
// A model that stops after `**bold wor` left a span open. The reader has two
// options: show the markers literally, or show the content as bold and wait
// for the closer to arrive. This module picks the second, but only when it can
// prove the choice was safe.
//
// The safety argument is the whole design. Every candidate repair is proven by
// parsing it, not predicted from a hand-rolled model of the delimiter rules:
// the engine appends a closer, reparses, and keeps the result only if the block
// count is unchanged, every block before the last is byte-identical, and the
// last block's text is exactly the original text minus the marker run it
// claimed to close. A repair that cannot be proven is not made, so the default
// answer is always the literal one.
//
// Nested spans close innermost first, one proven closer at a time, so
// `**bold *ital` reads as bold and italic. A candidate that fails its proof is
// skipped, not fatal, and a tail costs at most a few probes.
//
// Scope is deliberately narrow, and narrower than the library it is ported
// from. See README.md for the full list of what was left out and why.

#include "MarkdownParser.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace downstream::remend {

/// Which repairs the policy is allowed to attempt. Nothing is enabled by
/// default: an empty set means the policy is inert and every span stays
/// literal, which is what the engine did before this existed.
enum class PreviewFeature : uint32_t {
  /// A `*`, `_`, `~` or `` ` `` run that md4c left as literal text, closed
  /// provisionally so its content shows styled.
  Emphasis = 1u << 0,
  /// Retained separately from Emphasis so a caller can style code spans
  /// differently, and so the inline-code case has its own name in a test.
  InlineCode = 1u << 1,
};

struct PreviewOptions {
  using features_t = uint32_t;

  features_t features{0};

  bool enabled() const { return features != 0; }
  bool has(PreviewFeature feature) const { return (features & static_cast<features_t>(feature)) != 0; }

  /// Every feature, which is what `preview: true` asks for.
  static PreviewOptions all() {
    PreviewOptions options;
    options.features = allFeatures();
    return options;
  }

  /// Every bit this build defines. A caller that passes anything else gets the
  /// bits it meant, not a silently inert policy.
  static constexpr features_t allFeatures() {
    return static_cast<features_t>(PreviewFeature::Emphasis) | static_cast<features_t>(PreviewFeature::InlineCode);
  }
};

struct PreviewStats {
  /// Candidate closers appended and parsed.
  uint64_t probes{0};
  /// Closers that passed the safety proof and were kept. A tail with nested
  /// open spans can apply more than one.
  uint64_t applied{0};
  /// Tails where a span looked open but no candidate could be proven safe.
  uint64_t refused{0};
};

struct PreviewResult {
  /// The parse to publish. Equal to a parse of the unrepaired region whenever
  /// nothing was applied, so a caller can use it unconditionally.
  ParseResult parsed;
  /// The repaired text, or the region itself when nothing was applied.
  std::string text;
  /// True when `text` is not the region that was passed in.
  bool repaired{false};
  PreviewStats stats;
  /// Bytes handed to md4c: the region once, plus every probe. With no features
  /// enabled this is exactly the region, so an inert pass costs one parse.
  uint64_t bytesParsed{0};
};

class Preview {
public:
  explicit Preview(PreviewOptions options = PreviewOptions{}) : options_(options) {}

  bool enabled() const { return options_.enabled(); }
  const PreviewOptions &options() const { return options_; }

  /// Parses `region`, the unstable tail of a stream, and returns a preview.
  ///
  /// `origin` is the region's byte offset in the canonical source. It only
  /// matters to md4c's block-record arithmetic; see parseMarkdownAt.
  ///
  /// `frozenBlocks` counts leading blocks of the parse that a reader has seen
  /// settle, as a full parse over a committed prefix has. A repair is refused
  /// when the last block is one of them.
  ///
  /// Never throws and never mutates the caller's state, so it is safe to call
  /// from the worker pool and to construct once per session.
  PreviewResult apply(std::string_view region, size_t origin, size_t frozenBlocks = 0) const;

private:
  PreviewOptions options_;
};

} // namespace downstream::remend
