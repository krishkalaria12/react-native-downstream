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
@end

@implementation DownstreamMarkdownView {
  DSDocumentView *_documentView;
  DownstreamMarkdownShadowNode::ConcreteState::Shared _state;
  downstream::DocumentHandle _document;
  CGFloat _fontScale;
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
  const uint64_t nextDocumentId = data.document ? data.document->id : 0;
  if (_traceDocumentId && _traceDocumentId != nextDocumentId) {
    downstream::SessionTrace::recordDocument(_traceDocumentId, "frame_unavailable");
    [_traceDisplayLink invalidate];
    _traceDisplayLink = nil;
    _traceDocumentId = 0;
  }
  _document = data.document;
  _fontScale = data.fontSizeMultiplier;
  const uint64_t documentId = _document ? _document->id : 0;
  const BOOL tracing = documentId && downstream::SessionTrace::enabledDocument(documentId);
  if (tracing) downstream::SessionTrace::recordDocument(documentId, "apply_start");
  _documentView.rendered =
      _document ? [DSRenderedDocument renderedDocumentFor:_document fontScale:_fontScale] : nil;
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

- (void)updateProps:(const Props::Shared &)props oldProps:(const Props::Shared &)oldProps {
  const auto &newProps = *std::static_pointer_cast<const DownstreamMarkdownViewProps>(props);
  const auto &previous = *std::static_pointer_cast<const DownstreamMarkdownViewProps>(oldProps);
  if (newProps.sessionId != previous.sessionId || newProps.generation != previous.generation) {
    [_documentView resetTableScrollPositions];
  }
  _documentView.selectable = newProps.selectable;
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
