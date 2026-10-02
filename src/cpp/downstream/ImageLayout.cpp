#include "ImageLayout.h"

#include <algorithm>
#include <cctype>

namespace downstream {

namespace {

/// True when `text` starts with `prefix`, ignoring case.
///
/// Case-insensitive because a host name is, and a prefix list an app wrote as
/// `HTTPS://…` means the same thing. Not Unicode-aware on purpose: a prefix is
/// compared byte for byte or not at all, and folding a multi-byte character
/// would make a rule that cannot be explained.
bool startsWithIgnoringCase(std::string_view text, std::string_view prefix) {
  if (text.size() < prefix.size()) {
    return false;
  }
  for (size_t i = 0; i < prefix.size(); i++) {
    if (std::tolower(static_cast<unsigned char>(text[i])) !=
        std::tolower(static_cast<unsigned char>(prefix[i]))) {
      return false;
    }
  }
  return true;
}

/// The scale that fits `size` into a box, never more than 1.
float fitScale(float size, float limit) {
  if (!(size > 0) || !(limit > 0) || size <= limit) {
    return 1;
  }
  return limit / size;
}

} // namespace

const ImageEntry *ImageStateLayout::find(uint32_t ordinal) const {
  // Binary search over a vector the platforms and the shadow node keep sorted,
  // which is the same contract `hasChoice` in CodeLayout.h has.
  const auto found = std::lower_bound(
      entries.begin(), entries.end(), ordinal,
      [](const ImageEntry &entry, uint32_t value) { return entry.ordinal < value; });
  return found != entries.end() && found->ordinal == ordinal ? &*found : nullptr;
}

ImageBox imageBox(const ImageEntry *entry, float columnWidth, const ImageMetrics &metrics) {
  ImageBox box;
  const float column = columnWidth > 0 ? columnWidth : 0;

  if (entry == nullptr) {
    // No answer for this image yet, which is a view that was handed a document
    // it has not read. The placeholder is the honest answer: it is what the
    // reader would see a frame later.
    box.width = std::min(metrics.placeholderWidth, column);
    box.height = std::min(metrics.placeholderHeight, column);
    return box;
  }

  switch (entry->state) {
    case ImageState::Pending:
    case ImageState::Loading: {
      // A size known before the bytes arrive is as good as a loaded size: the
      // cache can hold a size for a URL this view has never loaded, which is
      // what makes a scrolled-back message lay out at its final height on its
      // first frame.
      if (entry->pixelWidth > 0 && entry->pixelHeight > 0) {
        break;
      }
      box.width = std::min(metrics.placeholderWidth, column);
      box.height = std::min(metrics.placeholderHeight, column);
      return box;
    }
    case ImageState::Failed:
    case ImageState::Blocked:
    case ImageState::Disabled:
      // One line of prose, so the text after a broken image does not move when
      // the load fails and the placeholder collapses.
      box.width = column;
      box.height = metrics.fallbackTextHeight;
      return box;
    case ImageState::Loaded:
      break;
  }

  const float width = entry->pixelWidth > 0 ? entry->pixelWidth : metrics.placeholderWidth;
  const float height = entry->pixelHeight > 0 ? entry->pixelHeight : metrics.placeholderHeight;
  // A column narrower than the image scales it down, and a cap does the same.
  // Neither ever scales up: a small image at its own size is the picture.
  const float scale = std::min(fitScale(width, column), fitScale(height, metrics.maxHeight));
  box.width = width * scale;
  box.height = height * scale;
  return box;
}

ImageState imageStateFor(bool enabled, bool urlLoadable, bool settled, bool known) {
  if (!enabled) {
    return ImageState::Disabled;
  }
  if (!urlLoadable) {
    // A destination that failed the engine's policy, or an empty one. An empty
    // destination is not an error while the writer is still typing, so it is
    // the document's settled count that says whether to show a box.
    return settled ? ImageState::Disabled : ImageState::Pending;
  }
  return known ? ImageState::Loaded : ImageState::Loading;
}

bool imageUrlAllowed(std::string_view url, const std::vector<std::string> &prefixes) {
  for (const std::string &prefix : prefixes) {
    if (prefix == "*") {
      return true;
    }
    if (!prefix.empty() && startsWithIgnoringCase(url, prefix)) {
      return true;
    }
  }
  return false;
}

} // namespace downstream
