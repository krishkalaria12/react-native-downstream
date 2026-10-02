#pragma once

#include "CodeLayout.h"
#include "Controls.h"
#include "Document.h"
#include "DocumentRegistry.h"
#include "ImageLayout.h"
#include "RenderPreparer.h"
#include "theme/Theme.h"

#include <react/renderer/graphics/Float.h>

#include <algorithm>
#include <cstdint>
#include <vector>

#ifdef RN_SERIALIZABLE_STATE
#include <folly/dynamic.h>
#endif

namespace facebook::react {

/// The chrome a document lays out with, from the props and the reader's
/// choices. Shared with the platforms so a measurement and a layout cannot
/// disagree about a height.
using downstream::chromeForProps;

/// The bits of the `controls` prop, so this layer reads `ControlBits::CodeCopy`
/// next to the rest of the Fabric code.
namespace ControlBits = downstream::ControlBits;

/// Mounting state: the parsed document, the font scale it was measured with,
/// and the code layout the reader is at. The view renders exactly this, so a
/// mounted frame never mixes content from one revision with a size from another.
class DownstreamMarkdownState final {
public:
  DownstreamMarkdownState() = default;

  DownstreamMarkdownState(downstream::DocumentHandle document, Float fontSizeMultiplier, uint32_t chrome = 0,
                          downstream::CodeLayout code = {}, downstream::Theme::Handle theme = nullptr,
                          downstream::ImageStateLayout images = {})
      : document(std::move(document)),
        fontSizeMultiplier(fontSizeMultiplier),
        chrome(chrome),
        code(std::move(code)),
        theme(std::move(theme)),
        images(std::move(images)) {}

  downstream::DocumentHandle document;
  Float fontSizeMultiplier{1.0};
  /// See `chromeForProps`. The resolved value travels with the state so the view
  /// lays out at the height the node measured.
  uint32_t chrome{downstream::kChromeNone};
  /// The code layout: the `lineNumbers`, `codeBlockWrap`, and `codeBlockMaxHeight`
  /// props, plus the reader's own per-block wrap and expand choices.
  ///
  /// The choices are the reader's state rather than a revision of it, because
  /// the platforms need the decisions themselves, not a number that says they
  /// changed. `chrome` cannot stand in for them: it says every code block wraps
  /// or that not all of them do, and a document with a mix needs the sets.
  downstream::CodeLayout code;
  /// The theme the node measured with. It travels with the state so a mounted
  /// frame cannot mix sizes from one theme with drawing from another, the same
  /// way `chrome` does. Null means the default theme.
  downstream::Theme::Handle theme;
  /// What this view knows about each image: whether images are on, and each
  /// one's state and pixel size.
  ///
  /// It lives in the state rather than in the document because it is a property
  /// of a view and of a moment, not of the Markdown. A load finishing changes an
  /// entry, the shadow node notices, and the frame is re-measured and re-mounted
  /// with no React render — which is the same path a code block's expand takes.
  downstream::ImageStateLayout images;

  /// The theme to draw with, resolving null to the default so a platform never
  /// has to check.
  const downstream::Theme::Handle &resolvedTheme() const {
    return theme ? theme : downstream::Theme::defaultTheme();
  }

  /// The theme's id, or 0 for the default one, which is its id too.
  uint64_t themeId() const { return theme ? theme->id() : 0; }

  /// The layout of the code block at `ordinal`, from the code layout and that
  /// block's own fence meta. The one definition both platforms call.
  downstream::CodeBlockLayout layoutOf(uint32_t ordinal, const downstream::FenceMeta &meta) const {
    return downstream::resolveCodeBlock(code, ordinal, meta);
  }

  /// A copy with `ordinal` toggled and its chrome resolved again.
  DownstreamMarkdownState toggledWrap(uint32_t ordinal, uint32_t controls) const {
    DownstreamMarkdownState next(*this);
    downstream::toggleChoice(next.code.choices.wrapToggles, ordinal);
    next.chrome = chromeForProps(controls, next.code);
    return next;
  }

  /// A copy with the cap lifted for `ordinal`, or put back, and its chrome
  /// resolved again.
  DownstreamMarkdownState toggledExpand(uint32_t ordinal, uint32_t controls) const {
    DownstreamMarkdownState next(*this);
    downstream::toggleChoice(next.code.choices.expanded, ordinal);
    next.chrome = chromeForProps(controls, next.code);
    return next;
  }

  /// A copy with the same choices and a chrome resolved for different props,
  /// which is what a change to `controls` or a code layout prop needs.
  DownstreamMarkdownState recoded(uint32_t controls, downstream::CodeLayoutProps props) const {
    DownstreamMarkdownState next(*this);
    next.code.props = props;
    next.chrome = chromeForProps(controls, next.code);
    return next;
  }

  /// A copy with new image answers, which is what a finished load produces.
  DownstreamMarkdownState withImages(downstream::ImageStateLayout answers) const {
    DownstreamMarkdownState next(*this);
    next.images = std::move(answers);
    return next;
  }

#ifdef RN_SERIALIZABLE_STATE
  /// The view hands a new state back through StateWrapper, so the choices the
  /// reader just made have to survive the round trip through JNI. The arrays are
  /// read back sorted, because the platform resolves them by binary search.
  DownstreamMarkdownState(const DownstreamMarkdownState &previous, folly::dynamic data)
      : document(previous.document),
        fontSizeMultiplier(previous.fontSizeMultiplier),
        chrome(previous.chrome),
        code(previous.code),
        theme(previous.theme),
        images(previous.images) {
    if (!data.isObject()) {
      return;
    }
    if (auto value = data.get_ptr("chrome")) {
      chrome = static_cast<uint32_t>(value->asDouble());
    }
    if (auto value = data.get_ptr("codeBlockWrap")) {
      code.props.wrap = value->asDouble() != 0;
    }
    if (auto value = data.get_ptr("lineNumbers")) {
      code.props.lineNumbers = value->asDouble() != 0;
    }
    if (auto value = data.get_ptr("codeBlockMaxHeight")) {
      code.props.maxHeight = static_cast<float>(value->asDouble());
    }
    if (auto value = data.get_ptr("wrapToggles")) {
      code.choices.wrapToggles = readOrdinals(*value);
    }
    if (auto value = data.get_ptr("expanded")) {
      code.choices.expanded = readOrdinals(*value);
    }
    // The image answers cross as one flat array: the enabled flag, then four
    // numbers per image. One array rather than an array of arrays, because a JNI
    // array of arrays allocates one object per image on a state update, and a
    // message with six charts would pay six allocations for six numbers. Read
    // back sorted by ordinal, which is what `find` binary-searches for.
    if (auto value = data.get_ptr("images")) {
      readImages(*value, &images);
    }
  }

  /// Java resolves the document through the native registry by id rather than
  /// copying the render model across JNI on every update.
  ///
  /// The booleans travel as numbers, so the whole state crosses JNI as one type.
  folly::dynamic getDynamic() const {
    downstream::DocumentRegistry::add(document);
    return folly::dynamic::object("documentId", document ? static_cast<double>(document->id) : 0.0)(
        "fontScale", static_cast<double>(fontSizeMultiplier))("chrome", static_cast<double>(chrome))(
        // The booleans travel as numbers, so the whole state crosses JNI as one
        // type.
        "codeBlockWrap", code.props.wrap ? 1.0 : 0.0)("lineNumbers", code.props.lineNumbers ? 1.0 : 0.0)(
        "codeBlockMaxHeight", static_cast<double>(code.props.maxHeight))(
        "wrapToggles", writeOrdinals(code.choices.wrapToggles))("expanded", writeOrdinals(code.choices.expanded))(
        // By id, as the document is: Java resolves the theme through the
        // registry rather than copying its values across JNI on every update.
        "themeId", static_cast<double>(themeId()))(
        // Flat, so the whole answer is one array: the enabled flag, then five
        // numbers per image — ordinal, state, width, height, and a trailing zero
        // that keeps every entry the same size, which is what lets the reader
        // index it without knowing how many images there are.
        "images", writeImages(images));
  }

private:
  /// The image answers, flattened. The stride is `kImageEntryStride` and both
  /// halves of the round trip use it, so a field added to `ImageEntry` is one
  /// line in each rather than a format that can drift.
  static constexpr size_t kImageEntryStride = 4;

  static folly::dynamic writeImages(const downstream::ImageStateLayout &images) {
    auto array = folly::dynamic::array();
    array.push_back(images.enabled ? 1.0 : 0.0);
    for (const downstream::ImageEntry &entry : images.entries) {
      array.push_back(static_cast<double>(entry.ordinal));
      array.push_back(static_cast<double>(entry.state));
      array.push_back(static_cast<double>(entry.pixelWidth));
      array.push_back(static_cast<double>(entry.pixelHeight));
    }
    return array;
  }

  static void readImages(const folly::dynamic &array, downstream::ImageStateLayout *out) {
    out->entries.clear();
    if (!array.isArray() || array.empty()) {
      return;
    }
    out->enabled = array[0].asDouble() != 0;
    out->entries.reserve((array.size() - 1) / kImageEntryStride);
    for (size_t at = 1; at + kImageEntryStride <= array.size(); at += kImageEntryStride) {
      downstream::ImageEntry entry;
      entry.ordinal = static_cast<uint32_t>(array[at].asDouble());
      entry.state = static_cast<downstream::ImageState>(static_cast<uint8_t>(array[at + 1].asDouble()));
      entry.pixelWidth = static_cast<float>(array[at + 2].asDouble());
      entry.pixelHeight = static_cast<float>(array[at + 3].asDouble());
      out->entries.push_back(entry);
    }
    std::sort(out->entries.begin(), out->entries.end(),
              [](const downstream::ImageEntry &a, const downstream::ImageEntry &b) {
                return a.ordinal < b.ordinal;
              });
  }

  static std::vector<uint32_t> readOrdinals(const folly::dynamic &array) {
    std::vector<uint32_t> ordinals;
    if (!array.isArray()) {
      return ordinals;
    }
    ordinals.reserve(array.size());
    for (const auto &entry : array) {
      ordinals.push_back(static_cast<uint32_t>(entry.asDouble()));
    }
    std::sort(ordinals.begin(), ordinals.end());
    return ordinals;
  }

  static folly::dynamic writeOrdinals(const std::vector<uint32_t> &ordinals) {
    auto array = folly::dynamic::array();
    for (const uint32_t ordinal : ordinals) {
      array.push_back(static_cast<double>(ordinal));
    }
    return array;
  }
#endif
};

} // namespace facebook::react
