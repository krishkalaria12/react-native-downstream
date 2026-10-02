#include "DownstreamMarkdownShadowNode.h"

#include "ImageLayout.h"
#include "ImageSizeCache.h"
#include "MarkdownParser.h"
#include "RenderPreparer.h"
#include "SessionRegistry.h"

#include <algorithm>

#include <react/renderer/core/LayoutContext.h>
#include <react/renderer/core/StateUpdate.h>

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
             previous.sessionSequence == props.sessionSequence && previous.sessionRevision == props.sessionRevision) {
    controlled_ = source.controlled_;
    // A revision that showed a fallback while its source was being prepared
    // collects the result in the next revision.
    needsDocument_ = source.needsDocument_ || source.awaitingPrepared_;
    pendingError_ = source.pendingError_;
  } else if (props.sessionId.empty() && previous.markdown == props.markdown) {
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
    controlled_->prepared->noteHint(layoutHint(1, 0));
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

downstream::LayoutHint DownstreamMarkdownShadowNode::layoutHint(Float fontScale, Float width) const {
  const auto &state = getStateData();
  return downstream::LayoutHint{static_cast<float>(fontScale),
                                static_cast<float>(width),
                                chromeFor(state),
                                codeLayout(state),
                                imageLayout(getDocument()),
                                theme_};
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
  controlled_->prepared = downstream::PreparedDocument::request(props.markdown, layoutHint(1, 0));
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

/// The code layout the props and the state's choices together describe.
downstream::CodeLayout DownstreamMarkdownShadowNode::codeLayout(const DownstreamMarkdownState &state) const {
  const auto &props = getConcreteProps();
  downstream::CodeLayout code;
  // Measuring reads the props rather than the state's copy of them, because a
  // prop change lands in props a revision before the state that records it does.
  code.props = {props.codeBlockWrap, props.lineNumbers, static_cast<float>(props.codeBlockMaxHeight)};
  code.choices = state.code.choices;
  return code;
}

uint32_t DownstreamMarkdownShadowNode::chromeFor(const DownstreamMarkdownState &state) const {
  return chromeForProps(getConcreteProps().controls, codeLayout(state));
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

Size DownstreamMarkdownShadowNode::measureContent(const LayoutContext &layoutContext,
                                                  const LayoutConstraints &layoutConstraints) const {
  const auto &props = getConcreteProps();
  const auto &state = getStateData();
  const uint32_t chrome = chromeFor(state);
  // The layout is the props' defaults plus the reader's own choices, so a hint
  // carries both. Measuring reads the props rather than the state's copy of them,
  // because a code layout prop change lands in props a revision before the state
  // that records it does. The hint carries the theme too, so the worker builds
  // the rendering this measurement will mount rather than one Fabric discards.
  const downstream::LayoutHint hint =
      layoutHint(layoutContext.fontSizeMultiplier, layoutConstraints.maximumSize.width);
  downstream::RenderPreparer::noteLayout(hint);
  if (!props.sessionId.empty()) {
    downstream::SessionRegistry::noteLayout(props.sessionId, hint);
  }
  return layoutManager_->measure(getDocument(), layoutContext.fontSizeMultiplier, layoutConstraints,
                                 getSurfaceId(), chrome, codeLayout(state), imageLayout(getDocument()), theme_);
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
  const downstream::CodeLayout code = codeLayout(state);
  // The image answers are part of this comparison for the same reason the code
  // layout is: the node measured with one set and would otherwise mount with
  // another, which is a picture that draws at a height it was not measured at.
  const downstream::ImageStateLayout images = imageState(imageLayout(document));
  if (state.document != document || state.fontSizeMultiplier != layoutContext.fontSizeMultiplier ||
      state.chrome != chrome || state.code != code || state.images != images || state.themeId() != themeId()) {
    DownstreamMarkdownState next{state};
    next.document = document;
    next.fontSizeMultiplier = layoutContext.fontSizeMultiplier;
    next.chrome = chrome;
    next.code = code;
    next.images = images;
    next.theme = theme_;
    setStateData(std::move(next));
  }

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
