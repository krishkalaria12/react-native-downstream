#import "DownstreamMarkdownView.h"

#import "DSRenderedDocument.h"
#import "DSSegmentViews.h"

#import <React/RCTConversions.h>
#import <react/renderer/components/DownstreamSpec/EventEmitters.h>
#import <react/renderer/components/DownstreamSpec/Props.h>
#import <react/renderer/components/DownstreamSpec/RCTComponentViewHelpers.h>

#include "DownstreamMarkdownComponentDescriptor.h"

using namespace facebook::react;

@interface DownstreamMarkdownView () <RCTDownstreamMarkdownViewViewProtocol, DSLinkDelegate>
@end

@implementation DownstreamMarkdownView {
  DSDocumentView *_documentView;
  DownstreamMarkdownShadowNode::ConcreteState::Shared _state;
  downstream::DocumentHandle _document;
  CGFloat _fontScale;
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
  _document = data.document;
  _fontScale = data.fontSizeMultiplier;
  _documentView.rendered =
      _document ? [DSRenderedDocument renderedDocumentFor:_document fontScale:_fontScale] : nil;
}

- (void)updateProps:(const Props::Shared &)props oldProps:(const Props::Shared &)oldProps {
  const auto &newProps = *std::static_pointer_cast<const DownstreamMarkdownViewProps>(props);
  _documentView.selectable = newProps.selectable;
  [super updateProps:props oldProps:oldProps];
}

- (void)prepareForRecycle {
  [super prepareForRecycle];
  _state.reset();
  _document.reset();
  _documentView.rendered = nil;
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
