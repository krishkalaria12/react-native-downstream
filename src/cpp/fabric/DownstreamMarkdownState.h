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
                          downstream::CodeLayoutProps code = {}, downstream::Theme::Handle theme = nullptr,
                          downstream::PlacedCaret caret = {},
                          downstream::ImageStateLayout images = {},
                          downstream::HostLayout hosts = {},
                          downstream::TableLayoutProps tables = {},
                          downstream::ReaderChoices choices = {})
      : document(std::move(document)),
        fontSizeMultiplier(fontSizeMultiplier),
        chrome(chrome),
        code(std::move(code)),
        theme(std::move(theme)),
        caret(caret),
        images(std::move(images)),
        hosts(std::move(hosts)),
        tables(std::move(tables)),
        choices(std::move(choices)) {}

  downstream::DocumentHandle document;
  Float fontSizeMultiplier{1.0};
  /// See `chromeForProps`. The resolved value travels with the state so the view
  /// lays out at the height the node measured.
  uint32_t chrome{downstream::kChromeNone};
  /// The code block's props: `lineNumbers`, `codeBlockWrap`, and
  /// `codeBlockMaxHeight`. The reader's own choices are beside it in `choices`
  /// rather than inside it, because they are the same set for every kind of
  /// collapsible block and one set is what travels in one state update.
  downstream::CodeLayoutProps code;
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
  /// The table's prop: `tableMaxHeight`.
  ///
  /// In the state for the reason `code` is: a table's height is a decision the
  /// view and the measurement have to make the same way, and the reader's choice
  /// is state rather than a revision of it, so it reaches the platform through
  /// the one path a native view has for changing its own layout.
  downstream::TableLayoutProps tables;
  /// The reader's own choices, every kind together: which code blocks wrap or
  /// are uncapped, which tables are uncapped, and which collapsible blocks are
  /// open.
  ///
  /// The choices rather than a revision of them, because the platforms need the
  /// decisions themselves, not a number that says they changed. `chrome` cannot
  /// stand in for them: it says every code block wraps or that not all of them
  /// do, and a document with a mix needs the sets.
  downstream::ReaderChoices choices;

  /// The code bundle for `code` and `choices`, which is what the platforms
  /// resolve a block against.
  downstream::CodeLayout codeLayout() const { return downstream::codeLayout(code, choices); }
  /// The table bundle, likewise.
  downstream::TableLayout tableLayout() const { return downstream::tableLayout(tables, choices); }

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
    return downstream::resolveCodeBlock(codeLayout(), ordinal, meta);
  }

  /// Whether the code block at `ordinal` is drawn by a React child rather than
  /// by the native code card. Both platforms ask this before building a card.
  bool hostsBlock(uint32_t ordinal) const { return hosts.hosts(ordinal); }

  /// A copy with `ordinal` toggled and its chrome resolved again.
  DownstreamMarkdownState toggledWrap(uint32_t ordinal, uint32_t controls) const {
    DownstreamMarkdownState next(*this);
    next.choices = next.choices.toggled(downstream::ChoiceKind::Wrap, ordinal);
    next.chrome = chromeForProps(controls, next.code, next.choices);
    return next;
  }

  /// A copy with the cap lifted for `ordinal`, or put back, and its chrome
  /// resolved again.
  DownstreamMarkdownState toggledExpand(uint32_t ordinal, uint32_t controls) const {
    DownstreamMarkdownState next(*this);
    next.choices = next.choices.toggled(downstream::ChoiceKind::CodeExpand, ordinal);
    next.chrome = chromeForProps(controls, next.code, next.choices);
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
    next.choices = next.choices.toggled(downstream::ChoiceKind::TableExpand, ordinal);
    return next;
  }

  /// A copy with one collapsible block's body shown or hidden.
  ///
  /// Its own method for the same reason `toggledTableExpand` is one: the
  /// ordinal counts collapsible blocks, so a call that took a bare ordinal would
  /// not say which of the reader's four sets it meant.
  DownstreamMarkdownState toggledDetails(uint32_t ordinal) const {
    DownstreamMarkdownState next(*this);
    next.choices = next.choices.toggled(downstream::ChoiceKind::DetailsOpen, ordinal);
    return next;
  }

  /// A copy with a new table cap, which is what the `tableMaxHeight` prop needs.
  DownstreamMarkdownState withTableCap(downstream::TableLayoutProps props) const {
    DownstreamMarkdownState next(*this);
    next.tables = props;
    return next;
  }

  /// The layout of the table at `ordinal`. The one definition both platforms
  /// call, beside `layoutOf` for a code block.
  ///
  /// `writtenOpen` is the writer's `open` attribute, which the reader's own
  /// answer overrides either way.
  downstream::TableBlockLayout tableLayoutOf(uint32_t ordinal) const {
    return downstream::resolveTable(tableLayout(), ordinal);
  }

  /// A copy with the same choices and a chrome resolved for different props,
  /// which is what a change to `controls` or a code layout prop needs.
  DownstreamMarkdownState recoded(uint32_t controls, downstream::CodeLayoutProps props) const {
    DownstreamMarkdownState next(*this);
    next.code = props;
    next.chrome = chromeForProps(controls, next.code, next.choices);
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
        tables(previous.tables),
        choices(previous.choices) {
    if (!data.isObject()) {
      return;
    }
    if (auto value = data.get_ptr("chrome")) {
      chrome = static_cast<uint32_t>(value->asDouble());
    }
    if (auto value = data.get_ptr("codeBlockWrap")) {
      code.wrap = value->asDouble() != 0;
    }
    if (auto value = data.get_ptr("lineNumbers")) {
      code.lineNumbers = value->asDouble() != 0;
    }
    if (auto value = data.get_ptr("codeBlockMaxHeight")) {
      code.maxHeight = static_cast<float>(value->asDouble());
    }
    if (auto value = data.get_ptr("wrapToggles")) {
      choices.wrapToggles = readOrdinals(*value);
    }
    if (auto value = data.get_ptr("expanded")) {
      choices.codeExpanded = readOrdinals(*value);
    }
    if (auto value = data.get_ptr("tableMaxHeight")) {
      tables.maxHeight = static_cast<float>(value->asDouble());
    }
    if (auto value = data.get_ptr("tableExpanded")) {
      choices.tableExpanded = readOrdinals(*value);
    }
    if (auto value = data.get_ptr("detailsOpen")) {
      choices.detailsOpen = readOrdinals(*value);
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
    // The hosted tags, beside the hosted fences: a container id rather than a
    // code ordinal, because the two count different things.
    if (auto value = data.get_ptr("tagHostEntries")) {
      readTagHosts(*value, &hosts);
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
        "codeBlockWrap", code.wrap ? 1.0 : 0.0)("lineNumbers", code.lineNumbers ? 1.0 : 0.0)(
        "codeBlockMaxHeight", static_cast<double>(code.maxHeight))(
        "wrapToggles", writeOrdinals(choices.wrapToggles))("expanded", writeOrdinals(choices.codeExpanded))(
        // The table cap and the reader's choices, beside the code block's. Every
        // choice set travels in one map, which is what lets a details toggle
        // reach the platforms without a fourth state key of its own kind.
        "tableMaxHeight", static_cast<double>(tables.maxHeight))(
        "tableExpanded", writeOrdinals(choices.tableExpanded))("detailsOpen", writeOrdinals(choices.detailsOpen))(
        // The caret's two small enums travel as numbers, so the whole state is
        // still one type crossing JNI.
        "caretKind", static_cast<double>(caret.kind))("caretPlace", static_cast<double>(caret.place))(
        // By id, as the document is: Java resolves the theme through the
        // registry rather than copying its values across JNI on every update.
        "themeId", static_cast<double>(themeId()))(
        // The same keys the Android measure map uses, because the view reads
        // this state and that map through one function.
        "imagesEnabled", images.enabled ? 1.0 : 0.0)("imageEntries", writeImages(images))("hostEntries", writeHosts(hosts))(
        "tagHostEntries", writeTagHosts(hosts));
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

  /// One object per hosted tag, as the measure map writes them: a container id
  /// rather than a code ordinal, because the two count different things.
  static void readTagHosts(const folly::dynamic &array, downstream::HostLayout *out) {
    out->tags.clear();
    if (!array.isArray()) {
      return;
    }
    out->tags.reserve(array.size());
    for (const auto &object : array) {
      if (!object.isObject()) {
        continue;
      }
      const auto *container = object.get_ptr("container");
      const auto *height = object.get_ptr("height");
      if (container == nullptr || height == nullptr || !container->isNumber() || !height->isNumber()) {
        continue;
      }
      downstream::TagHostEntry entry;
      entry.container = static_cast<uint32_t>(container->asDouble());
      entry.height = static_cast<float>(height->asDouble());
      out->tags.push_back(entry);
    }
    std::sort(out->tags.begin(), out->tags.end(),
              [](const downstream::TagHostEntry &a, const downstream::TagHostEntry &b) {
                return a.container < b.container;
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

  static folly::dynamic writeTagHosts(const downstream::HostLayout &hosts) {
    auto array = folly::dynamic::array();
    for (const downstream::TagHostEntry &entry : hosts.tags) {
      array.push_back(folly::dynamic::object("container", static_cast<double>(entry.container))(
          "height", static_cast<double>(entry.height)));
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
