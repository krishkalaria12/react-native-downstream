#pragma once

#include "Caret.h"
#include "CodeLayout.h"
#include "Controls.h"
#include "Document.h"
#include "DocumentRegistry.h"
#include "HostLayout.h"
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
                          downstream::PlacedCaret caret = {},
                          downstream::ImageStateLayout images = {},
                          downstream::HostLayout hosts = {},
                          downstream::TableLayout tables = {})
      : document(std::move(document)),
        fontSizeMultiplier(fontSizeMultiplier),
        chrome(chrome),
        code(std::move(code)),
        theme(std::move(theme)),
        caret(caret),
        images(std::move(images)),
        hosts(std::move(hosts)),
        tables(std::move(tables)) {}

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
  /// The caret the view draws, or that there is none. It is a height as much as
  /// a mark: the placeholder sits in the measured text of the last segment, so
  /// the shadow node measures with it and the view draws with it for the same
  /// reason `chrome` does.
  downstream::PlacedCaret caret;
  /// What this view knows about each image: whether images are on, and each
  /// one's state and pixel size.
  ///
  /// It lives in the state rather than in the document because it is a property
  /// of a view and of a moment, not of the Markdown. A load finishing changes an
  /// entry, the shadow node notices, and the frame is re-measured and re-mounted
  /// with no React render — which is the same path a code block's expand takes.
  downstream::ImageStateLayout images;
  /// Every React child this view mounts into a fence, and the height it
  /// measured at. A code segment whose ordinal is in here draws its container
  /// chrome and nothing else, and takes the child's height.
  ///
  /// It travels in the state for the reason `images` does: a child that
  /// changes size is a change to this view and not to the Markdown, and it is
  /// re-measured and re-mounted without a React render.
  downstream::HostLayout hosts;
  /// The table layout: the `tableMaxHeight` prop and the reader's own per-table
  /// expand choices.
  ///
  /// In the state for the reason `code` is: a table's height is a decision the
  /// view and the measurement have to make the same way, and the reader's choice
  /// is state rather than a revision of it, so it reaches the platform through
  /// the one path a native view has for changing its own layout.
  downstream::TableLayout tables;

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

  /// Whether the code block at `ordinal` is drawn by a React child rather than
  /// by the native code card. Both platforms ask this before building a card.
  bool hostsBlock(uint32_t ordinal) const { return hosts.hosts(ordinal); }

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

  /// A copy with the cap lifted for one *table*, or put back.
  ///
  /// Its own method rather than an argument on `toggledExpand`, because the two
  /// choices are keyed by different ordinals: a code ordinal counts code blocks
  /// and a table ordinal counts tables, so one call that took both would need to
  /// know which list a caller meant and could get it wrong silently.
  DownstreamMarkdownState toggledTableExpand(uint32_t ordinal) const {
    DownstreamMarkdownState next(*this);
    downstream::toggleChoice(next.tables.choices.expanded, ordinal);
    return next;
  }

  /// A copy with a new table cap, which is what the `tableMaxHeight` prop needs.
  DownstreamMarkdownState withTableCap(downstream::TableLayoutProps props) const {
    DownstreamMarkdownState next(*this);
    next.tables.props = props;
    return next;
  }

  /// The layout of the table at `ordinal`. The one definition both platforms
  /// call, beside `layoutOf` for a code block.
  downstream::TableBlockLayout tableLayoutOf(uint32_t ordinal) const {
    return downstream::resolveTable(tables, ordinal);
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

  /// A copy with new hosted-child heights, which is what a React child that
  /// changed size produces on the next measurement.
  DownstreamMarkdownState withHosts(downstream::HostLayout answers) const {
    DownstreamMarkdownState next(*this);
    next.hosts = std::move(answers);
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
        caret(previous.caret),
        images(previous.images),
        hosts(previous.hosts),
        tables(previous.tables) {
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
    if (auto value = data.get_ptr("tableMaxHeight")) {
      tables.props.maxHeight = static_cast<float>(value->asDouble());
    }
    if (auto value = data.get_ptr("tableExpanded")) {
      tables.choices.expanded = readOrdinals(*value);
    }
    if (auto value = data.get_ptr("caretKind")) {
      caret.kind = static_cast<downstream::CaretKind>(static_cast<int>(value->asDouble()));
    }
    if (auto value = data.get_ptr("caretPlace")) {
      caret.place = static_cast<downstream::CaretPlace>(static_cast<int>(value->asDouble()));
    }
    // The image answers, in the shape `codeLayoutData` writes on Android, so
    // the measure map, the state and a round trip all read one set of keys.
    if (auto value = data.get_ptr("imagesEnabled")) {
      images.enabled = value->asDouble() != 0;
    }
    if (auto value = data.get_ptr("imageEntries")) {
      readImages(*value, &images);
    }
    // The hosted children, in the shape the measure map writes them, so the
    // state, the measure map and a round trip all read one set of keys.
    if (auto value = data.get_ptr("hostEntries")) {
      readHosts(*value, &hosts);
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
        // The table cap and its reader's choices, beside the code block's: a table
        // is the second block kind with a height of its own.
        "tableMaxHeight", static_cast<double>(tables.props.maxHeight))(
        "tableExpanded", writeOrdinals(tables.choices.expanded))(
        // The caret's two small enums travel as numbers, so the whole state is
        // still one type crossing JNI.
        "caretKind", static_cast<double>(caret.kind))("caretPlace", static_cast<double>(caret.place))(
        // By id, as the document is: Java resolves the theme through the
        // registry rather than copying its values across JNI on every update.
        "themeId", static_cast<double>(themeId()))(
        // The same keys the Android measure map uses, because the view reads
        // this state and that map through one function.
        "imagesEnabled", images.enabled ? 1.0 : 0.0)("imageEntries", writeImages(images))("hostEntries", writeHosts(hosts));
  }

private:
  /// One object per image, as `codeLayoutData` writes them.
  static folly::dynamic writeImages(const downstream::ImageStateLayout &images) {
    auto array = folly::dynamic::array();
    for (const downstream::ImageEntry &entry : images.entries) {
      array.push_back(folly::dynamic::object("ordinal", static_cast<double>(entry.ordinal))(
          "state", static_cast<double>(entry.state))("pixelWidth", static_cast<double>(entry.pixelWidth))(
          "pixelHeight", static_cast<double>(entry.pixelHeight)));
    }
    return array;
  }

  static void readImages(const folly::dynamic &array, downstream::ImageStateLayout *out) {
    out->entries.clear();
    if (!array.isArray()) {
      return;
    }
    out->entries.reserve(array.size());
    const auto number = [](const folly::dynamic &object, const char *key) {
      const auto *value = object.get_ptr(key);
      return value != nullptr && value->isNumber() ? value->asDouble() : 0.0;
    };
    for (const auto &object : array) {
      if (!object.isObject()) {
        continue;
      }
      downstream::ImageEntry entry;
      entry.ordinal = static_cast<uint32_t>(number(object, "ordinal"));
      entry.state = static_cast<downstream::ImageState>(static_cast<uint8_t>(number(object, "state")));
      entry.pixelWidth = static_cast<float>(number(object, "pixelWidth"));
      entry.pixelHeight = static_cast<float>(number(object, "pixelHeight"));
      out->entries.push_back(entry);
    }
    std::sort(out->entries.begin(), out->entries.end(),
              [](const downstream::ImageEntry &a, const downstream::ImageEntry &b) {
                return a.ordinal < b.ordinal;
              });
  }

  static void readHosts(const folly::dynamic &array, downstream::HostLayout *out) {
    out->entries.clear();
    if (!array.isArray()) {
      return;
    }
    out->entries.reserve(array.size());
    for (const auto &object : array) {
      if (!object.isObject()) {
        continue;
      }
      const auto *ordinal = object.get_ptr("ordinal");
      const auto *height = object.get_ptr("height");
      if (ordinal == nullptr || height == nullptr || !ordinal->isNumber() || !height->isNumber()) {
        continue;
      }
      downstream::HostEntry entry;
      entry.ordinal = static_cast<uint32_t>(ordinal->asDouble());
      entry.height = static_cast<float>(height->asDouble());
      out->entries.push_back(entry);
    }
    std::sort(out->entries.begin(), out->entries.end(),
              [](const downstream::HostEntry &a, const downstream::HostEntry &b) {
                return a.ordinal < b.ordinal;
              });
  }

  /// One object per hosted child, as the measure map writes them.
  static folly::dynamic writeHosts(const downstream::HostLayout &hosts) {
    auto array = folly::dynamic::array();
    for (const downstream::HostEntry &entry : hosts.entries) {
      array.push_back(
          folly::dynamic::object("ordinal", static_cast<double>(entry.ordinal))("height", static_cast<double>(entry.height)));
    }
    return array;
  }

  static std::vector<uint32_t> readOrdinals(const folly::dynamic &array) {    std::vector<uint32_t> ordinals;
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
