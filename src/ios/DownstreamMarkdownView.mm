#import "DownstreamMarkdownView.h"

#import "DSRenderedDocument.h"
#import "DSSegmentViews.h"
#import <QuartzCore/CADisplayLink.h>

#import <React/RCTConversions.h>
#import <react/renderer/components/DownstreamSpec/EventEmitters.h>
#import <react/renderer/components/DownstreamSpec/Props.h>
#import <react/renderer/components/DownstreamSpec/RCTComponentViewHelpers.h>

#include "DownstreamMarkdownComponentDescriptor.h"
#include "SessionTrace.h"

using namespace facebook::react;

@interface DownstreamMarkdownView () <RCTDownstreamMarkdownViewViewProtocol, DSLinkDelegate>
- (void)scheduleTraceFrame;
- (void)applyRendering;
@end

@implementation DownstreamMarkdownView {
  DSDocumentView *_documentView;
  DownstreamMarkdownShadowNode::ConcreteState::Shared _state;
  downstream::DocumentHandle _document;
  CGFloat _fontScale;
  BOOL _syntaxHighlighting;
  CADisplayLink *_traceDisplayLink;
  uint64_t _traceDocumentId;
}

+ (ComponentDescriptorProvider)componentDescriptorProvider {
  return concreteComponentDescriptorProvider<DownstreamMarkdownViewComponentDescriptor>();
}

- (instancetype)initWithFrame:(CGRect)frame {
  if (self = [super initWithFrame:frame]) {
    static const auto defaultProps = std::make_shared<const DownstreamMarkdownViewProps>();
    _props = defaultProps;

    _syntaxHighlighting = YES;
    _documentView = [[DSDocumentView alloc] initWithFrame:self.bounds];
    _documentView.linkDelegate = self;
    self.contentView = _documentView;

    UITapGestureRecognizer *tap = [[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(handleTap:)];
    tap.cancelsTouchesInView = NO;
    [self addGestureRecognizer:tap];
  }
  return self;
}

- (void)updateState:(const State::Shared &)state oldState:(const State::Shared &)oldState {
  _state = std::static_pointer_cast<const DownstreamMarkdownShadowNode::ConcreteState>(state);
  if (!_state) {
    return;
  }
  const auto &data = _state->getData();
  if (data.document == _document && data.fontSizeMultiplier == _fontScale) {
    return;
  }
  _document = data.document;
  _fontScale = data.fontSizeMultiplier;
  const uint64_t documentId = _document ? _document->id : 0;
  const BOOL tracing = documentId && downstream::SessionTrace::enabledDocument(documentId);
  if (tracing) downstream::SessionTrace::recordDocument(documentId, "apply_start");
  // applyRendering ends the trace the previous document was waiting on.
  [self applyRendering];
  if (tracing) {
    downstream::SessionTrace::recordDocument(documentId, "apply_end");
    _traceDocumentId = documentId;
    [self scheduleTraceFrame];
  }
}

- (void)scheduleTraceFrame {
  if (!_traceDocumentId || _traceDisplayLink || !self.window) return;
  _traceDisplayLink = [CADisplayLink displayLinkWithTarget:self selector:@selector(traceDisplayTick:)];
  [_traceDisplayLink addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
}

- (void)didMoveToWindow {
  [super didMoveToWindow];
  [self scheduleTraceFrame];
}

- (void)traceDisplayTick:(CADisplayLink *)link {
  [link invalidate];
  if (_traceDisplayLink != link) return;
  _traceDisplayLink = nil;
  if (_document && _document->id == _traceDocumentId && self.window) {
    // A display-link tick follows the mount; it is an estimate, not a GPU paint timestamp.
    downstream::SessionTrace::recordDocument(_traceDocumentId, "frame_estimate");
    _traceDocumentId = 0;
  }
}

/// Rebuilds the platform text from the document the view already holds, and
/// ends any frame trace the old rendering was waiting on.
///
/// A color appearance change deliberately does not come through here. Token
/// colors are dynamic UIColors, which the platform resolves against the trait
/// collection when it draws, so a cached rendering already follows the
/// appearance and rebuilding it would only discard warm caches to get the same
/// objects back.
- (void)applyRendering {
  const uint64_t documentId = _document ? _document->id : 0;
  if (_traceDocumentId && _traceDocumentId != documentId) {
    downstream::SessionTrace::recordDocument(_traceDocumentId, "frame_unavailable");
    [_traceDisplayLink invalidate];
    _traceDisplayLink = nil;
    _traceDocumentId = 0;
  }
  _documentView.rendered = _document ? [DSRenderedDocument renderedDocumentFor:_document
                                                                 fontScale:_fontScale
                                                        syntaxHighlighting:_syntaxHighlighting]
                                     : nil;
}

- (void)updateProps:(const Props::Shared &)props oldProps:(const Props::Shared &)oldProps {
  const auto &newProps = *std::static_pointer_cast<const DownstreamMarkdownViewProps>(props);
  // Fabric passes no old props when it mounts a new or recycled view; the
  // view's current props are the previous value then.
  const auto &previous = *std::static_pointer_cast<const DownstreamMarkdownViewProps>(oldProps ? oldProps : _props);
  if (newProps.sessionId != previous.sessionId || newProps.generation != previous.generation) {
    [_documentView resetTableScrollPositions];
  }
  _documentView.selectable = newProps.selectable;
  // Tokens are always computed, so the prop only chooses the rendering. The
  // document's own tokens are unchanged, so nothing needs re-parsing.
  if (newProps.syntaxHighlighting != previous.syntaxHighlighting) {
    _syntaxHighlighting = newProps.syntaxHighlighting;
    // Tokens are computed whatever the flag says, so this only picks the
    // rendering, and the cache keys the two apart.
    [self applyRendering];
  }
  [super updateProps:props oldProps:oldProps];
}

- (void)prepareForRecycle {
  if (_traceDocumentId) downstream::SessionTrace::recordDocument(_traceDocumentId, "frame_unavailable");
  _traceDocumentId = 0;
  [_traceDisplayLink invalidate];
  _traceDisplayLink = nil;
  [super prepareForRecycle];
  _state.reset();
  _document.reset();
  _documentView.rendered = nil;
}

- (void)dealloc {
  if (_traceDocumentId) downstream::SessionTrace::recordDocument(_traceDocumentId, "frame_unavailable");
  [_traceDisplayLink invalidate];
}

- (void)handleTap:(UITapGestureRecognizer *)recognizer {
  if (recognizer.state != UIGestureRecognizerStateEnded) {
    return;
  }
  const NSInteger linkIndex = [_documentView linkIndexAtPoint:[recognizer locationInView:_documentView]];
  if (linkIndex >= 0) {
    [self didPressLinkAtIndex:linkIndex];
  }
}

- (void)didPressLinkAtIndex:(NSInteger)linkIndex {
  if (!_state || !_eventEmitter) {
    return;
  }
  const auto &document = _state->getData().document;
  if (!document || linkIndex < 0 || static_cast<size_t>(linkIndex) >= document->links.size()) {
    return;
  }
  const auto &link = document->links[static_cast<size_t>(linkIndex)];
  std::static_pointer_cast<const DownstreamMarkdownViewEventEmitter>(_eventEmitter)
      ->onLinkPress({
          .url = link.url,
          .start = static_cast<int>(link.range.start),
          .end = static_cast<int>(link.range.end),
      });
}

@end

Class<RCTComponentViewProtocol> DownstreamMarkdownViewCls(void) {
  return DownstreamMarkdownView.class;
}
