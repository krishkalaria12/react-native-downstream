#include "DownstreamMarkdownShadowNode.h"

#include "MarkdownParser.h"
#include "RenderPreparer.h"
#include "SessionRegistry.h"

#include <react/renderer/core/LayoutContext.h>
#include <react/renderer/core/StateUpdate.h>

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
}

void DownstreamMarkdownShadowNode::requestControlledSource() {
  needsDocument_ = true;
  const auto &props = getConcreteProps();
  if (!props.sessionId.empty()) {
    controlled_.reset();
    return;
  }
  controlled_ = std::make_shared<ControlledSource>();
  controlled_->prepared = downstream::PreparedDocument::request(props.markdown);
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

uint32_t DownstreamMarkdownShadowNode::chromeFor(const DownstreamMarkdownState &state) const {
  const auto &props = getConcreteProps();
  return chromeForProps(props.controls, props.codeBlockWrap, state.wrapToggles);
}

Size DownstreamMarkdownShadowNode::measureContent(const LayoutContext &layoutContext,
                                                  const LayoutConstraints &layoutConstraints) const {
  const auto &props = getConcreteProps();
  const auto &state = getStateData();
  const uint32_t chrome = chromeFor(state);
  // The layout is the prop's default plus the reader's own choices, so a hint
  // carries both. Measuring reads the prop rather than the state's copy of it,
  // because a `codeBlockWrap` change lands in props a revision before the state
  // that records it does.
  const downstream::LayoutHint hint{static_cast<float>(layoutContext.fontSizeMultiplier),
                                    static_cast<float>(layoutConstraints.maximumSize.width),
                                    chrome,
                                    state.wrapToggles,
                                    props.codeBlockWrap};
  downstream::RenderPreparer::noteLayout(hint);
  if (!props.sessionId.empty()) {
    downstream::SessionRegistry::noteLayout(props.sessionId, hint);
  }
  return layoutManager_->measure(getDocument(), layoutContext.fontSizeMultiplier, layoutConstraints,
                                 getSurfaceId(), chrome, props.codeBlockWrap, state.wrapToggles);
}

void DownstreamMarkdownShadowNode::layout(LayoutContext layoutContext) {
  ensureUnsealed();
  ConcreteViewShadowNode::layout(layoutContext);

  const auto &document = getDocument();
  if (awaitingPrepared_) {
    relayoutWhenPrepared();
  }
  const auto &props = getConcreteProps();
  const auto &state = getStateData();
  const uint32_t chrome = chromeFor(state);
  if (state.document != document || state.fontSizeMultiplier != layoutContext.fontSizeMultiplier ||
      state.chrome != chrome || state.codeBlockWrap != props.codeBlockWrap) {
    DownstreamMarkdownState next{state};
    next.document = document;
    next.fontSizeMultiplier = layoutContext.fontSizeMultiplier;
    next.chrome = chrome;
    next.codeBlockWrap = props.codeBlockWrap;
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

} // namespace facebook::react
