#import "DownstreamMarkdownView.h"

#import "DSMarkdownTextView.h"

#import <React/RCTConversions.h>
#import <react/renderer/components/DownstreamSpec/EventEmitters.h>
#import <react/renderer/components/DownstreamSpec/Props.h>
#import <react/renderer/components/DownstreamSpec/RCTComponentViewHelpers.h>

#include "DownstreamMarkdownComponentDescriptor.h"

using namespace facebook::react;

@interface DownstreamMarkdownView () <RCTDownstreamMarkdownViewViewProtocol, DSMarkdownTextViewDelegate>
@end

@implementation DownstreamMarkdownView {
  DSMarkdownTextView *_textView;
  DownstreamMarkdownShadowNode::ConcreteState::Shared _state;
}

+ (ComponentDescriptorProvider)componentDescriptorProvider {
  return concreteComponentDescriptorProvider<DownstreamMarkdownViewComponentDescriptor>();
}

- (instancetype)initWithFrame:(CGRect)frame {
  if (self = [super initWithFrame:frame]) {
    static const auto defaultProps = std::make_shared<const DownstreamMarkdownViewProps>();
    _props = defaultProps;

    _textView = [[DSMarkdownTextView alloc] initWithFrame:self.bounds];
    _textView.delegate = self;
    self.contentView = _textView;

    UITapGestureRecognizer *tap = [[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(handleTap:)];
    tap.cancelsTouchesInView = NO;
    [self addGestureRecognizer:tap];
  }
  return self;
}

- (void)updateState:(const State::Shared &)state oldState:(const State::Shared &)oldState {
  _state = std::static_pointer_cast<const DownstreamMarkdownShadowNode::ConcreteState>(state);
  if (_state) {
    const auto &data = _state->getData();
    [_textView setDocument:data.document fontScale:data.fontSizeMultiplier];
  }
}

- (void)prepareForRecycle {
  [super prepareForRecycle];
  _state.reset();
  [_textView setDocument:nullptr fontScale:1];
}

- (void)handleTap:(UITapGestureRecognizer *)recognizer {
  if (recognizer.state != UIGestureRecognizerStateEnded) {
    return;
  }
  const NSInteger linkIndex = [_textView linkIndexAtPoint:[recognizer locationInView:_textView]];
  if (linkIndex >= 0) {
    [self markdownTextView:_textView didPressLinkAtIndex:linkIndex];
  }
}

- (void)markdownTextView:(DSMarkdownTextView *)view didPressLinkAtIndex:(NSInteger)linkIndex {
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
