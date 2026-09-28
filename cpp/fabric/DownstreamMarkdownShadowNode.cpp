#include "DownstreamMarkdownShadowNode.h"

#include "MarkdownParser.h"
#include "SessionRegistry.h"

#include <react/renderer/core/LayoutContext.h>

namespace facebook::react {

extern const char DownstreamMarkdownViewComponentName[] = "DownstreamMarkdownView";

DownstreamMarkdownShadowNode::DownstreamMarkdownShadowNode(const ShadowNode &sourceShadowNode,
                                                           const ShadowNodeFragment &fragment)
    : ConcreteViewShadowNode(sourceShadowNode, fragment) {
  const auto &source = static_cast<const DownstreamMarkdownShadowNode &>(sourceShadowNode);
  layoutManager_ = source.layoutManager_;

  // Keep the last valid document either way: it is reused when the source is
  // unchanged, and retained as the fallback when a new source fails to parse.
  document_ = source.document_;
  if (source.getConcreteProps().sessionId != getConcreteProps().sessionId ||
      (!getConcreteProps().sessionId.empty() &&
       source.getConcreteProps().generation != getConcreteProps().generation)) {
    document_.reset();
    needsParse_ = true;
  } else if (source.getConcreteProps().markdown == getConcreteProps().markdown &&
             source.getConcreteProps().generation == getConcreteProps().generation &&
             source.getConcreteProps().sessionSequence == getConcreteProps().sessionSequence &&
             source.getConcreteProps().sessionRevision == getConcreteProps().sessionRevision) {
    needsParse_ = source.needsParse_;
    pendingError_ = source.pendingError_;
  } else {
    needsParse_ = true;
  }
}

void DownstreamMarkdownShadowNode::setLayoutManager(
    std::shared_ptr<const DownstreamTextLayoutManager> layoutManager) {
  ensureUnsealed();
  layoutManager_ = std::move(layoutManager);
}

const downstream::DocumentHandle &DownstreamMarkdownShadowNode::getDocument() const {
  if (needsParse_) {
    needsParse_ = false;
    const auto &props = getConcreteProps();
    if (!props.sessionId.empty()) {
      // A session document is prepared on its worker. Measurement never parses
      // streaming input and never reads a snapshot from a different version.
      auto ready = downstream::SessionRegistry::snapshot(props.sessionId, props.generation,
                                                          props.sessionSequence);
      if (ready) {
        document_ = std::move(ready);
      }
      return document_;
    }
    auto result = downstream::parseMarkdown(getConcreteProps().markdown);
    if (result.status == downstream::ParseStatus::Ok) {
      document_ = std::move(result.document);
      pendingError_.reset();
    } else {
      pendingError_ = PendingError{downstream::errorCode(result.status), std::move(result.message)};
    }
  }
  return document_;
}

Size DownstreamMarkdownShadowNode::measureContent(const LayoutContext &layoutContext,
                                                  const LayoutConstraints &layoutConstraints) const {
  return layoutManager_->measure(getDocument(), layoutContext.fontSizeMultiplier, layoutConstraints, getSurfaceId());
}

void DownstreamMarkdownShadowNode::layout(LayoutContext layoutContext) {
  ensureUnsealed();
  ConcreteViewShadowNode::layout(layoutContext);

  const auto &document = getDocument();
  const auto &state = getStateData();
  if (state.document != document || state.fontSizeMultiplier != layoutContext.fontSizeMultiplier) {
    setStateData(DownstreamMarkdownState{document, layoutContext.fontSizeMultiplier});
  }

  if (pendingError_) {
    getConcreteEventEmitter().onMarkdownError({
        .code = pendingError_->code,
        .message = pendingError_->message,
        .generation = getConcreteProps().generation,
        .sequence = 0,
        .recoverable = true,
    });
    pendingError_.reset();
  }
}

} // namespace facebook::react
