#include "DownstreamMarkdownShadowNode.h"

#include "TagPolicy.h"

#include "ImageLayout.h"
#include "ImageSizeCache.h"
#include "MarkdownParser.h"
#include "RenderPreparer.h"
#include "SegmentPlan.h"
#include "SessionRegistry.h"
#include "theme/ThemeLayout.h"

#include <algorithm>

#include <react/renderer/components/DownstreamSpec/ShadowNodes.h>
#include <react/renderer/core/LayoutContext.h>
#include <react/renderer/core/StateUpdate.h>
#include <react/renderer/graphics/Rect.h>

#include <utility>

namespace facebook::react {

extern const char DownstreamMarkdownViewComponentName[] = "DownstreamMarkdownView";

namespace {

// Layout waits for a smaller controlled source, which is usually prepared by
// then, so the first frame already shows it. A larger one arrives a frame or
// more later instead of stalling the JavaScript thread.
constexpr size_t kWaitForPreparedBytes = 16 * 1024;

} // namespace

DownstreamMarkdownShadowNode::DownstreamMarkdownShadowNode(const ShadowNodeFragment &fragment,
                                                           const ShadowNodeFamily::Shared &family,
                                                           ShadowNodeTraits traits)
    : ConcreteViewShadowNode(fragment, family, traits) {
  // The theme is interned once per distinct value, so an unchanged inline theme
  // literal costs a pointer compare per revision rather than a hash.
  resolveTheme();
  requestControlledSource();
}

DownstreamMarkdownShadowNode::DownstreamMarkdownShadowNode(const ShadowNode &sourceShadowNode,
                                                           const ShadowNodeFragment &fragment)
    : ConcreteViewShadowNode(sourceShadowNode, fragment) {
  const auto &source = static_cast<const DownstreamMarkdownShadowNode &>(sourceShadowNode);
  const auto &previous = source.getConcreteProps();
  const auto &props = getConcreteProps();
  layoutManager_ = source.layoutManager_;
  // The reader's wrap choices are not held here: they live in the state, which
  // Fabric passes to every clone of this node, so a state update that clones
  // the node keeps them without a second copy that could fall out of step.

  // Source preparation needs the current theme before scheduling its worker.
  const bool sameTheme = themeUnchanged(previous);
  if (sameTheme) {
    theme_ = source.theme_;
  } else {
    resolveTheme();
  }

  // Keep the last valid document either way: it is reused when the source is
  // unchanged, and retained as the fallback when a new source fails to parse.
  document_ = source.document_;
  if (previous.sessionId != props.sessionId || (!props.sessionId.empty() && previous.generation != props.generation)) {
    document_.reset();
    requestControlledSource();
  } else if (previous.markdown == props.markdown && previous.generation == props.generation &&
             previous.sessionSequence == props.sessionSequence && previous.sessionRevision == props.sessionRevision &&
             previous.parseFlags == props.parseFlags && previous.allowedTags == props.allowedTags) {
    controlled_ = source.controlled_;
    // A revision that showed a fallback while its source was being prepared
    // collects the result in the next revision.
    needsDocument_ = source.needsDocument_ || source.awaitingPrepared_;
    pendingError_ = source.pendingError_;
  } else if (props.sessionId.empty() && previous.markdown == props.markdown &&
             previous.parseFlags == props.parseFlags && previous.allowedTags == props.allowedTags) {
    // A generation change without a new source, after a rejected value.
    controlled_ = source.controlled_;
  } else {
    requestControlledSource();
  }

  // A clone can reuse pending source work while changing its theme or controls.
  // The worker can use this hint if it has not collected its layouts yet.
  // Later changes use the normal on-demand platform rendering cache.
  if (controlled_ && controlled_->prepared &&
      (!sameTheme || previous.controls != props.controls || previous.codeBlockWrap != props.codeBlockWrap)) {
    controlled_->prepared->noteHint(layoutHint(1, 0, document_));
  }
}

bool DownstreamMarkdownShadowNode::themeUnchanged(const DownstreamMarkdownViewProps &previous) const {
  const auto &props = getConcreteProps();
  return previous.themeSlots == props.themeSlots && previous.themeValues == props.themeValues &&
      previous.themeFonts == props.themeFonts;
}

downstream::Theme::Handle DownstreamMarkdownShadowNode::resolveTheme() const {
  const auto &props = getConcreteProps();
  if (props.themeSlots.empty()) {
    // An app that passes neither prop gets the static default, which costs
    // nothing beyond one pointer compare on every measurement.
    theme_.reset();
    return nullptr;
  }
  std::vector<uint32_t> slots;
  std::vector<double> values;
  // The slot and value arrays are parallel by construction, so a length that
  // does not match is a malformed prop rather than a value a caller can send on
  // purpose; the extra slots are dropped and the rest of the theme still lands.
  slots.reserve(props.themeSlots.size());
  values.reserve(std::min(props.themeSlots.size(), props.themeValues.size()));
  for (size_t index = 0; index < props.themeSlots.size() && index < props.themeValues.size(); index++) {
    slots.push_back(static_cast<uint32_t>(props.themeSlots[index]));
    values.push_back(props.themeValues[index]);
  }
  const std::vector<std::string> fonts(props.themeFonts.begin(), props.themeFonts.end());
  theme_ = downstream::ThemeRegistry::intern(slots, values, fonts);
  return theme_;
}

/**
 * The `dir` prop, as the engine's own direction.
 *
 * `Inherit` for `auto`, which is the engine's decision per block: a prop that
 * named a direction would be a second rule, and the whole point of deciding the
 * direction in C++ was that there is only one.
 */
downstream::BlockDirection directionFor(DownstreamMarkdownViewDir value) {
  switch (value) {
    case DownstreamMarkdownViewDir::Ltr:
      return downstream::BlockDirection::LeftToRight;
    case DownstreamMarkdownViewDir::Rtl:
      return downstream::BlockDirection::RightToLeft;
    case DownstreamMarkdownViewDir::Auto:
      break;
  }
  return downstream::BlockDirection::Inherit;
}

downstream::LayoutHint DownstreamMarkdownShadowNode::layoutHint(Float fontScale, Float width,
                                                               const downstream::DocumentHandle &document) const {
  const auto &state = getStateData();
  // The table's cap rides beside the code block's rather than inside it, because
  // a table's height is a different block kind's decision and mixing them would
  // make every code wrap toggle rebuild the table measurement. The reader's one
  // set of choices goes in whole: it is the same set for every kind, so there is
  // nothing to decide here beyond carrying it.
  downstream::LayoutHint hint{static_cast<float>(fontScale),
                              static_cast<float>(width),
                              chromeFor(state),
                              codeProps(state),
                              state.choices,
                              state.caret,
                              imageLayout(document),
                              theme_};
  hint.tables = tableProps(state);
  hint.direction = directionFor(getConcreteProps().dir);
  return hint;
}

/// The two small enums, mapped from the codegen ones.
///
/// The wire names are parsed once, by codegen's own `fromRawValue`, so this is a
/// table rather than a set of string comparisons: there is no string to compare
/// against by the time the node sees the prop, and a table keeps the two
/// vocabularies from drifting apart silently.
downstream::CaretKind caretKind(DownstreamMarkdownViewCaret value) {
  switch (value) {
    case DownstreamMarkdownViewCaret::Block:
      return downstream::CaretKind::Block;
    case DownstreamMarkdownViewCaret::Circle:
      return downstream::CaretKind::Circle;
    case DownstreamMarkdownViewCaret::None:
      break;
  }
  return downstream::CaretKind::None;
}

/**
 * The `dir` prop, as the engine's own direction.
 *
 * `Inherit` for `auto`, which is the engine's decision per block: a prop that
 * named a direction would be a second rule, and the whole point of deciding the
 * direction in C++ was that there is only one.
 */
downstream::Animating animatingFor(DownstreamMarkdownViewAnimating value) {
  switch (value) {
    case DownstreamMarkdownViewAnimating::Auto:
      return downstream::Animating::Auto;
    case DownstreamMarkdownViewAnimating::On:
      return downstream::Animating::On;
    case DownstreamMarkdownViewAnimating::Off:
      break;
  }
  return downstream::Animating::Off;
}

downstream::PlacedCaret DownstreamMarkdownShadowNode::caretFor(const downstream::DocumentHandle &document) const {
  const auto &props = getConcreteProps();
  downstream::CaretSpec spec;
  spec.kind = caretKind(props.caret);
  spec.animating = animatingFor(props.animating);
  // A stream's own `streaming` flag is set by the session that published the
  // snapshot, so an `auto` caret follows the stream without JavaScript knowing.
  if (!document) {
    return downstream::PlacedCaret{};
  }
  return downstream::resolveCaret(*document, spec);
}

void DownstreamMarkdownShadowNode::requestControlledSource() {
  needsDocument_ = true;
  const auto &props = getConcreteProps();
  if (!props.sessionId.empty()) {
    controlled_.reset();
    return;
  }
  controlled_ = std::make_shared<ControlledSource>();
  // The hint carries this node's theme. Its width is zero, because the node has
  // not been measured yet, and a hint with no width is expanded against the
  // widths views use now, so the theme is still prepared.
  // The view's math options are parse options: the same string parses to a
  // different document with math on, so they travel with the request and are
  // part of what the preparation cache keys on.
  // The same string parses to a different document under a different tag policy,
  // so it travels with the request and is part of what the cache keys on, beside
  // the flags. The list is the wire form `src/allowedTags.ts` writes.
  downstream::ParseOptions parse{static_cast<uint32_t>(props.parseFlags),
                                 downstream::tagPolicyFromStrings(props.allowedTags)};
  controlled_->prepared = downstream::PreparedDocument::request(props.markdown, layoutHint(1, 0, document_), parse);
}

void DownstreamMarkdownShadowNode::setLayoutManager(
    std::shared_ptr<const DownstreamTextLayoutManager> layoutManager) {
  ensureUnsealed();
  layoutManager_ = std::move(layoutManager);
}

const downstream::DocumentHandle &DownstreamMarkdownShadowNode::getDocument() const {
  if (!needsDocument_) {
    return document_;
  }
  const auto &props = getConcreteProps();
  if (!props.sessionId.empty()) {
    needsDocument_ = false;
    // A session document is prepared on the pool. Measurement never parses
    // streaming input and never reads a snapshot from a different version.
    auto ready = downstream::SessionRegistry::snapshot(props.sessionId, props.generation, props.sessionSequence);
    if (ready) {
      document_ = std::move(ready);
    }
    return document_;
  }

  needsDocument_ = false;
  const auto &prepared = controlled_ ? controlled_->prepared : nullptr;
  if (!prepared) {
    return document_;
  }
  if (!prepared->done()) {
    if (prepared->source().size() > kWaitForPreparedBytes) {
      // Measurement and mounting of this revision both use the fallback,
      // even if the preparation finishes in between.
      awaitingPrepared_ = true;
      return document_;
    }
    prepared->runOrWait();
  }
  const auto &result = prepared->result();
  if (result.status == downstream::ParseStatus::Ok && result.document) {
    document_ = result.document;
    pendingError_.reset();
  } else {
    pendingError_ = PendingError{downstream::errorCode(result.status), result.message};
  }
  return document_;
}

/**
 * Reports each formula that failed in `document` and had not failed in
 * `previous`.
 *
 * Only a settled formula has an error, because one that is still arriving is
 * not a failure yet. A block whose identity is unchanged from the previous
 * document already reported its formula, which is what keeps a stream from
 * sending the same error with every token; JavaScript also drops a repeat per
 * view, so this is a cost bound rather than the guarantee.
 */
void DownstreamMarkdownShadowNode::reportMathErrors(const downstream::DocumentHandle &document,
                                                    const downstream::DocumentHandle &previous) const {
  const int generation = getConcreteProps().generation;
  for (size_t index = 0; index < document->blocks.size(); index++) {
    const downstream::Block &block = document->blocks[index];
    if (block.math.empty()) {
      continue;
    }
    const downstream::Block *before =
        previous && index < previous->blocks.size() ? &previous->blocks[index] : nullptr;
    const bool sameBlock = before != nullptr && before->identity == block.identity &&
        before->math.size() == block.math.size();
    for (size_t span = 0; span < block.math.size(); span++) {
      const downstream::MathSpan &formula = block.math[span];
      if (formula.error.empty() || (sameBlock && !before->math[span].error.empty())) {
        continue;
      }
      getConcreteEventEmitter().onMathError({
          .source = formula.tex,
          .message = formula.error,
          .display = formula.display,
          .generation = generation,
      });
    }
  }
}

void DownstreamMarkdownShadowNode::relayoutWhenPrepared() const {
  if (controlled_->relayoutRequested.exchange(true)) {
    return;
  }
  // A state update clones this node in the current tree, which measures it
  // again; the clone then finds the preparation done. It targets the family,
  // so it reaches whichever revision of the node is mounted by then.
  std::weak_ptr<const ShadowNodeFamily> weakFamily = getFamilyShared();
  controlled_->prepared->onDone([weakFamily] {
    const auto family = weakFamily.lock();
    if (!family) {
      return;
    }
    family->dispatchRawState(
        StateUpdate{family,
                    [](const StateData::Shared &data) -> StateData::Shared {
                      return std::make_shared<const DownstreamMarkdownState>(
                          *std::static_pointer_cast<const DownstreamMarkdownState>(data));
                    }},
        EventQueue::UpdateMode::Asynchronous);
  });
}

/// The code block's props, which measurement reads rather than the state's copy
/// of them, because a prop change lands in props a revision before the state
/// that records it does.
downstream::CodeLayoutProps DownstreamMarkdownShadowNode::codeProps(const DownstreamMarkdownState &) const {
  const auto &props = getConcreteProps();
  return {props.codeBlockWrap, props.lineNumbers, static_cast<float>(props.codeBlockMaxHeight)};
}

/// The table's cap, read for the same reason `codeProps` reads the code ones.
downstream::TableLayoutProps DownstreamMarkdownShadowNode::tableProps(const DownstreamMarkdownState &) const {
  const auto &props = getConcreteProps();
  return {static_cast<float>(props.tableMaxHeight)};
}

uint32_t DownstreamMarkdownShadowNode::chromeFor(const DownstreamMarkdownState &state) const {
  return chromeForProps(getConcreteProps().controls, codeProps(state), state.choices);
}

/// The `allowedImagePrefixes` prop, as the list `imageUrlAllowed` reads.
///
/// Normalized once per measurement rather than per image: the prop is almost
/// always an inline literal from JavaScript, and lower-casing it here would mean
/// allocating a copy of it for every node in every commit. The comparison is
/// already case-insensitive.
downstream::ImageLayout DownstreamMarkdownShadowNode::imageLayout(
    const downstream::DocumentHandle &document) const {
  const auto &props = getConcreteProps();
  downstream::ImageLayout layout;
  layout.state.enabled = props.images;
  layout.allowedPrefixes.assign(props.allowedImagePrefixes.begin(), props.allowedImagePrefixes.end());
  if (!document || document->images.empty()) {
    return layout;
  }
  std::vector<uint32_t> ordinals;
  std::vector<std::string> urls;
  std::vector<bool> loadable;
  ordinals.reserve(document->images.size());
  urls.reserve(document->images.size());
  loadable.reserve(document->images.size());
  for (size_t index = 0; index < document->images.size(); index++) {
    const downstream::Image &image = document->images[index];
    ordinals.push_back(static_cast<uint32_t>(index));
    urls.push_back(image.url);
    loadable.push_back(image.loadable);
  }
  // The cache knows the size of anything this process has loaded, so a message
  // scrolled back into view measures at its final height on its first frame.
  layout.state = downstream::ImageSizeCache::stateLayoutFor(ordinals, urls, loadable, props.images);
  // The prefix list is a view policy, applied here where the box is decided, so
  // the document is the same whatever an app is willing to fetch. A blocked
  // image answers `Blocked` without a request ever existing, and a failed one
  // keeps its failure so a scrolled-back message does not refetch a 404.
  for (auto &entry : layout.state.entries) {
    if (!props.images) {
      entry.state = downstream::ImageState::Disabled;
      continue;
    }
    const bool urlLoadable = entry.ordinal < loadable.size() && loadable[entry.ordinal];
    if (!urlLoadable) {
      if (entry.state == downstream::ImageState::Loaded) {
        continue;
      }
      // A destination that failed the engine's policy is blocked outright. One
      // that is merely absent is pending while the writer is still typing and
      // disabled once the document settles, which is the difference between
      // "not finished" and "not loadable".
      if (entry.ordinal < urls.size() && !urls[entry.ordinal].empty()) {
        entry.state = downstream::ImageState::Blocked;
      } else {
        entry.state = document->isSettled(document->blocks.size()) ? downstream::ImageState::Disabled
                                                                  : downstream::ImageState::Pending;
      }
      continue;
    }
    if (entry.state == downstream::ImageState::Loaded || entry.state == downstream::ImageState::Failed) {
      continue;
    }
    const std::string &url = urls[entry.ordinal];
    if (!downstream::imageUrlAllowed(url, layout.allowedPrefixes)) {
      entry.state = downstream::ImageState::Blocked;
    }
  }
  return layout;
}

/**
 * Asks for a relayout once the view has new answers about its images.
 *
 * A state update clones this node in the current tree, which measures it again;
 * the clone rebuilds the image layout from the cache and finds a different one,
 * so it measures at the loaded height and commits a new state. No React render
 * is involved, which is the whole point: a load finishing is not a change to the
 * document, and treating it as one would re-render every message in a chat.
 *
 * Called by the platform after it records a finished load, exactly as
 * `relayoutWhenPrepared` is called when a parse finishes.
 */
void DownstreamMarkdownShadowNode::relayoutWhenImagesChange(downstream::ImageStateLayout answers) const {
  // Dispatched on the family rather than on `this`, because a state update
  // clones the node in the current tree and the clone is what measures. The
  // answers are captured by value because a load finishes long after the view
  // that asked for it.
  getFamilyShared()->dispatchRawState(
      StateUpdate{getFamilyShared(),
                  [answers = std::move(answers)](const StateData::Shared &data) -> StateData::Shared {
                    const auto &previous = *std::static_pointer_cast<const DownstreamMarkdownState>(data);
                    return std::make_shared<const DownstreamMarkdownState>(previous.withImages(answers));
                  }},
      EventQueue::UpdateMode::Asynchronous);
}

void DownstreamMarkdownShadowNode::imagesChanged(const std::shared_ptr<const ShadowNode> &node) {
  if (!node) {
    return;
  }
  const auto *concrete = dynamic_cast<const DownstreamMarkdownShadowNode *>(node.get());
  if (concrete == nullptr) {
    return;
  }
  const auto &document = concrete->getDocument();
  // A document with no image answers nothing, and dispatching a state update
  // that changes nothing would clone every node in a chat for no reason.
  if (!document || document->images.empty()) {
    return;
  }
  const downstream::ImageStateLayout answers = concrete->imageState(concrete->imageLayout(document));
  if (answers == concrete->getStateData().images) {
    return;
  }
  concrete->relayoutWhenImagesChange(answers);
}

std::vector<DownstreamMarkdownShadowNode::TagSlotChild> DownstreamMarkdownShadowNode::tagSlotChildren() const {
  std::vector<TagSlotChild> slots;
  const auto &children = getChildren();
  for (size_t index = 0; index < children.size(); index++) {
    const auto &child = children[index];
    // The same dedicated component fences use: a slot's whole job is to hold
    // the app's component, and whether it replaces a fence or a tag changes
    // which block range it stands in for, not how it is held.
    const auto *slot = dynamic_cast<const DownstreamFenceSlotShadowNode *>(child.get());
    if (slot == nullptr) {
      continue;
    }
    const int32_t container = slot->getConcreteProps().containerId;
    if (container < 0) {
      continue;
    }
    if (slot->getConcreteProps().yogaStyle.display() == yoga::Display::None) {
      continue;
    }
    slots.push_back({static_cast<uint32_t>(container), child, index});
  }
  return slots;
}

std::vector<DownstreamMarkdownShadowNode::SlotChild> DownstreamMarkdownShadowNode::slotChildren() const {
  std::vector<SlotChild> slots;
  const auto &children = getChildren();
  for (size_t index = 0; index < children.size(); index++) {
    const auto &child = children[index];
    // The dedicated component rather than an app's own view carrying the
    // ordinal some other way: a renderer can only ever be hosted through this,
    // so nothing an app renders itself can be mistaken for a slot.
    const auto *slot = dynamic_cast<const DownstreamFenceSlotShadowNode *>(child.get());
    if (slot == nullptr) {
      continue;
    }
    if (slot->getConcreteProps().containerId >= 0) {
      continue;
    }
    // React hides a suspended subtree with `display: none`, and a hidden child
    // is not a renderer any more: its fence draws as the code card, which is
    // what the fallback is for. Read from the props, where React writes it, and
    // not from the layout metrics: `layoutSlots` hides a slot whose fence is not
    // in the document it is laying out the same way, and reading that back here
    // would keep the slot hidden after its fence arrives.
    if (slot->getConcreteProps().yogaStyle.display() == yoga::Display::None) {
      continue;
    }
    slots.push_back({static_cast<uint32_t>(std::max(slot->getConcreteProps().ordinal, 0)), child, index});
  }
  return slots;
}

downstream::HostLayout DownstreamMarkdownShadowNode::measureSlots(const LayoutContext &layoutContext,
                                                                  const LayoutConstraints &layoutConstraints) const {
  const Float maximumWidth = layoutConstraints.maximumSize.width;
  downstream::HostLayout hosts;
  const std::vector<SlotChild> slots = slotChildren();
  const std::vector<TagSlotChild> tagSlots = tagSlotChildren();
  if (slots.empty() && tagSlots.empty()) {
    // No renderer is mounted: the measurement is the one a view with no
    // renderers has always made, and the cache key is the same empty host
    // layout it was before this existed.
    return hosts;
  }
  const downstream::DocumentHandle &document = getDocument();
  // Index code blocks once for every mounted child, including indented blocks
  // that consume an ordinal but cannot be hosted. The old per-child walk grew
  // quadratically with the number of fences in a message.
  std::vector<const downstream::Block *> codeBlocks;
  if (document) {
    uint32_t lastOrdinal = 0;
    for (const auto &slot : slots) {
      lastOrdinal = std::max(lastOrdinal, slot.ordinal);
    }
    for (const auto &block : document->blocks) {
      if (block.kind == downstream::BlockKind::CodeBlock) {
        codeBlocks.push_back(&block);
        if (codeBlocks.size() > lastOrdinal) {
          break;
        }
      }
    }
  }
  const auto &theme = theme_;
  const double scale = theme ? downstream::theme_layout::fontScale(*theme, layoutContext.fontSizeMultiplier) : 1;
  hosts.entries.reserve(slots.size());
  for (const SlotChild &slot : slots) {
    const auto *layoutable = dynamic_cast<const LayoutableShadowNode *>(slot.node.get());
    if (layoutable == nullptr) {
      continue;
    }
    // The exact column and an unbounded height, as Paragraph measures an
    // inline view: the child is a component of the message, and nothing below
    // it may constrain it.
    float column = static_cast<float>(maximumWidth);
    if (slot.ordinal < codeBlocks.size() && theme && std::isfinite(maximumWidth)) {
      const double leading = downstream::theme_layout::contentLeading(*theme, codeBlocks[slot.ordinal]->containers, scale);
      column = static_cast<float>(std::max(0.0, static_cast<double>(maximumWidth) - leading));
    }
    LayoutConstraints constraints{Size{column, 0}, Size{column, std::numeric_limits<Float>::infinity()},
                                 layoutConstraints.layoutDirection};
    const Size size = layoutable->measure(layoutContext, constraints);
    hosts.entries.push_back({slot.ordinal, static_cast<float>(size.height)});
  }
  std::sort(hosts.entries.begin(), hosts.entries.end(),
            [](const downstream::HostEntry &a, const downstream::HostEntry &b) { return a.ordinal < b.ordinal; });
  // Tag slots beside fence slots: a child that replaces a custom container is
  // measured at the container's column, exactly as a fence child is measured
  // at its block's. The entries land in the tag key space, so a fence can
  // never claim a tag's height.
  hosts.tags.reserve(tagSlots.size());
  // Index custom containers once for every mounted child, the way fences index
  // code blocks above: one walk, however many tags a message holds.
  uint32_t lastContainer = 0;
  for (const auto &slot : tagSlots) {
    lastContainer = std::max(lastContainer, slot.container);
  }
  std::vector<const downstream::Block *> tagBlocks;
  if (document) {
    for (const auto &block : document->blocks) {
      for (const auto &container : block.containers) {
        if (container.kind == downstream::ContainerKind::Custom && container.id <= lastContainer) {
          tagBlocks.push_back(&block);
          break;
        }
      }
      if (tagBlocks.size() > lastContainer) {
        break;
      }
    }
  }
  for (const TagSlotChild &slot : tagSlots) {
    const auto *layoutable = dynamic_cast<const LayoutableShadowNode *>(slot.node.get());
    if (layoutable == nullptr) {
      continue;
    }
    float column = static_cast<float>(maximumWidth);
    for (const downstream::Block *block : tagBlocks) {
      bool found = false;
      for (const auto &container : block->containers) {
        if (container.kind == downstream::ContainerKind::Custom && container.id == slot.container) {
          found = true;
          break;
        }
      }
      if (found && theme && std::isfinite(maximumWidth)) {
        const double leading =
            downstream::theme_layout::contentLeading(*theme, block->containers, scale);
        column = static_cast<float>(std::max(0.0, static_cast<double>(maximumWidth) - leading));
        break;
      }
    }
    LayoutConstraints constraints{Size{column, 0}, Size{column, std::numeric_limits<Float>::infinity()},
                                 layoutConstraints.layoutDirection};
    const Size size = layoutable->measure(layoutContext, constraints);
    hosts.tags.push_back({slot.container, static_cast<float>(size.height)});
  }
  std::sort(hosts.tags.begin(), hosts.tags.end(),
            [](const downstream::TagHostEntry &a, const downstream::TagHostEntry &b) {
              return a.container < b.container;
            });
  return hosts;
}

Size DownstreamMarkdownShadowNode::measureContent(const LayoutContext &layoutContext,
                                                  const LayoutConstraints &layoutConstraints) const {
  const auto &props = getConcreteProps();
  const auto &state = getStateData();
  const auto &document = getDocument();
  const uint32_t chrome = chromeFor(state);
  // The layout is the props' defaults plus the reader's own choices, so a hint
  // carries both. Measuring reads the props rather than the state's copy of them,
  // because a code layout prop change lands in props a revision before the state
  // that records it does. The hint carries the theme too, so the worker builds
  // the rendering this measurement will mount rather than one Fabric discards.
  const downstream::LayoutHint hint =
      layoutHint(layoutContext.fontSizeMultiplier, layoutConstraints.maximumSize.width, document);
  downstream::RenderPreparer::noteLayout(hint);
  if (!props.sessionId.empty()) {
    downstream::SessionRegistry::noteLayout(props.sessionId, hint);
  }
  // Each mounted renderer is measured before the document is, because a hosted
  // fence is as tall as its component: the component's height is an input to
  // the document's, exactly as a loaded image's box is.
  measuredHosts_ = measureSlots(layoutContext, layoutConstraints);
  return layoutManager_
      ->measure(document, layoutContext.fontSizeMultiplier, layoutConstraints, getSurfaceId(), chrome,
                codeProps(state), caretFor(document), imageLayout(document), measuredHosts_, theme_,
                tableProps(state), state.choices)
      .size;
}

void DownstreamMarkdownShadowNode::layoutSlots(LayoutContext layoutContext) {
  const std::vector<SlotChild> slots = slotChildren();
  if (slots.empty() && tagSlotChildren().empty()) {
    return;
  }
  const downstream::DocumentHandle &document = getDocument();
  const auto &state = getStateData();
  const Float width = getLayoutMetrics().frame.size.width;
  // The rectangles come from a measurement at the frame's own width and with the
  // heights this pass measured, the way Paragraph places its inline views. Not a
  // lookup under the measured constraint: Yoga rounds the frame to the pixel
  // grid, so the frame width is rarely bit-equal to the width measured at, and
  // the state's hosts can still be the previous pass's. When the two widths do
  // agree this is the cache hit that measurement just wrote.
  const LayoutConstraints frame{Size{width, 0}, Size{width, std::numeric_limits<Float>::infinity()},
                                getLayoutMetrics().layoutDirection};
  const DocumentMeasurement measurement =
      layoutManager_->measure(document, layoutContext.fontSizeMultiplier, frame, getSurfaceId(), chromeFor(state),
                              codeProps(state), caretFor(document), imageLayout(document), measuredHosts_, theme_,
                              tableProps(state), state.choices);
  if (measurement.slots.empty() && measurement.tagSlots.empty()) {
    return;
  }
  if (children_ == nullptr) {
    return;
  }
  // A copy of the child list, written back once every slot is placed: cloning a
  // child while walking the list would otherwise invalidate the walk.
  auto children = std::make_shared<std::vector<std::shared_ptr<const ShadowNode>>>(*children_);
  layoutTagSlots(layoutContext, measurement, children);
  for (const SlotChild &slot : slots) {
    const size_t index = slot.childIndex;
    // A leaf Yoga node lays its own children out, exactly as the text shadow
    // node lays its inline views out: the child is cloned, laid out at the size
    // the document gave it, and given the origin the document put it at.
    std::shared_ptr<ShadowNode> cloned = slot.node->clone({});
    auto *layoutable = dynamic_cast<LayoutableShadowNode *>(cloned.get());
    if (layoutable == nullptr) {
      continue;
    }
    const Rect *rect = measurement.slotFor(slot.ordinal);
    if (rect == nullptr) {
      // A slot whose ordinal has no hosted block in this document: a stale
      // child from a lagging render, or one that arrived while the source was
      // still being prepared. Nothing is drawn for it, which is what a clipped
      // inline view does.
      layoutable->setLayoutMetrics(LayoutMetrics{.frame = {}, .displayType = DisplayType::None});
      (*children)[index] = std::move(cloned);
      continue;
    }
    const Size size{rect->size.width, rect->size.height};
    LayoutConstraints constraints{size, size, getLayoutMetrics().layoutDirection};
    layoutable->layoutTree(layoutContext, constraints);
    LayoutMetrics metrics = layoutable->getLayoutMetrics();
    metrics.frame.size = size;
    metrics.frame.origin = rect->origin;
    // Shown again explicitly: a slot hidden while its fence was missing keeps
    // that metric through a clone, and a Yoga tree with nothing new to lay out
    // does not overwrite it.
    metrics.displayType = DisplayType::Flex;
    layoutable->setLayoutMetrics(std::move(metrics));
    (*children)[index] = std::move(cloned);
  }
  children_ = std::move(children);
}

void DownstreamMarkdownShadowNode::layoutTagSlots(
    LayoutContext layoutContext, const DocumentMeasurement &measurement,
    std::shared_ptr<std::vector<std::shared_ptr<const ShadowNode>>> &children) {
  const std::vector<TagSlotChild> slots = tagSlotChildren();
  for (const TagSlotChild &slot : slots) {
    const size_t index = slot.childIndex;
    std::shared_ptr<ShadowNode> cloned = (*children)[index]->clone({});
    auto *layoutable = dynamic_cast<LayoutableShadowNode *>(cloned.get());
    if (layoutable == nullptr) {
      continue;
    }
    const Rect *rect = measurement.tagSlotFor(slot.container);
    if (rect == nullptr) {
      // A slot whose container has no hosted range in this document: a stale
      // child from a lagging render, or one that arrived while the source was
      // still being prepared. Nothing is drawn for it, the way a fence slot
      // with no block is hidden.
      layoutable->setLayoutMetrics(LayoutMetrics{.frame = {}, .displayType = DisplayType::None});
      (*children)[index] = std::move(cloned);
      continue;
    }
    const Size size{rect->size.width, rect->size.height};
    LayoutConstraints constraints{size, size, getLayoutMetrics().layoutDirection};
    layoutable->layoutTree(layoutContext, constraints);
    LayoutMetrics metrics = layoutable->getLayoutMetrics();
    metrics.frame.size = size;
    metrics.frame.origin = rect->origin;
    metrics.displayType = DisplayType::Flex;
    layoutable->setLayoutMetrics(std::move(metrics));
    (*children)[index] = std::move(cloned);
  }
  // No write-back here: the caller owns the list and writes it back once both
  // passes are done, so a tag slot placed here is not overwritten by a fence
  // slot placed after it.
}

void DownstreamMarkdownShadowNode::layout(LayoutContext layoutContext) {
  ensureUnsealed();
  ConcreteViewShadowNode::layout(layoutContext);

  const auto &document = getDocument();
  if (awaitingPrepared_) {
    relayoutWhenPrepared();
  }
  const auto &state = getStateData();
  const uint32_t chrome = chromeFor(state);
  // The theme is compared by id rather than by pointer, so a view whose theme
  // resolved to an equal value it already had does not ask for a state update.
  const downstream::CodeLayoutProps code = codeProps(state);
  // The caret and the image answers are both part of this comparison for the
  // same reason the code layout is: the node measured with one set and would
  // otherwise mount with another, which is a caret or a picture drawn at a
  // height it was not measured at.
  const downstream::PlacedCaret caret = caretFor(document);
  const downstream::ImageStateLayout images = imageState(imageLayout(document));
  // The hosted children are part of it for the same reason again: a fence drawn
  // by a React component takes that component's height, so the view has to
  // mount with the same set the node measured. The measurement's own answers,
  // not a second measurement: a child measured twice at two widths would put
  // the document and the view at two different heights.
  const downstream::HostLayout hosts = measuredHosts_;
  // The table layout is in this comparison for the same reason the code one is:
  // a capped table measures at one height and an expanded one at another, and a
  // view that mounted with the wrong set would scroll a table by a frame it was
  // not laid out for.
  const downstream::TableLayoutProps tables = tableProps(state);
  if (document && state.document != document) {
    reportMathErrors(document, state.document);
  }
  if (state.document != document || state.fontSizeMultiplier != layoutContext.fontSizeMultiplier ||
      state.chrome != chrome || state.code != code || state.tables != tables || state.caret != caret ||
      state.images != images || state.hosts != hosts || state.themeId() != themeId()) {
    DownstreamMarkdownState next{state};
    next.document = document;
    next.fontSizeMultiplier = layoutContext.fontSizeMultiplier;
    next.chrome = chrome;
    next.code = code;
    next.tables = tables;
    next.caret = caret;
    next.images = images;
    next.hosts = hosts;
    next.theme = theme_;
    setStateData(std::move(next));
  }
  layoutSlots(layoutContext);

  if (pendingError_) {
    getConcreteEventEmitter().onMarkdownError({
        .code = pendingError_->code,
        .message = pendingError_->message,
        .generation = getConcreteProps().generation,
        .sequence = 0,
        .recoverable = true,
        // A parse or layout failure names no control, so JavaScript routes it
        // to the view's own onError.
        .block = std::string(),
        .action = std::string(),
    });
    pendingError_.reset();
  }
}

uint64_t DownstreamMarkdownShadowNode::themeId() const { return theme_ ? theme_->id() : 0; }

} // namespace facebook::react
