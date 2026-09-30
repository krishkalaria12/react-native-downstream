#import "DownstreamMarkdownView.h"

#import "DSFileExporter.h"
#import "DSRenderedDocument.h"
#import "DSSegmentViews.h"
#import <QuartzCore/CADisplayLink.h>

#import <React/RCTConvert.h>
#import <react/renderer/components/DownstreamSpec/EventEmitters.h>
#import <react/renderer/components/DownstreamSpec/Props.h>
#import <react/renderer/components/DownstreamSpec/RCTComponentViewHelpers.h>

#include "Controls.h"
#include "DownstreamMarkdownComponentDescriptor.h"
#include "Export.h"
#include "SegmentPlan.h"
#include "SessionTrace.h"
#include "WorkerPool.h"

#include <algorithm>
#include <exception>
#include <string>
#include <vector>

using namespace facebook::react;
using namespace downstream;

namespace {

/// Larger exports serialize on the worker pool. Include per-cell overhead in
/// the estimate so even a large table of empty cells leaves the main thread.
constexpr size_t kInlineExportBytes = 64 * 1024;
/// Clipboard writes complete on the main thread. A newer copy press, even in
/// another Markdown view, supersedes an older worker that is still running.
uint64_t copyRequestEpoch = 0;

NSString *toNSString(const std::u16string &text) {
  return [NSString stringWithCharacters:reinterpret_cast<const unichar *>(text.data()) length:text.size()];
}

NSString *toNSString(const std::string &text) {
  return [NSString stringWithUTF8String:text.c_str()] ?: @"";
}

DSExportFormat formatFor(NSString *format) {
  if ([format isEqualToString:@"csv"]) {
    return DSExportFormatCsv;
  }
  if ([format isEqualToString:@"tsv"]) {
    return DSExportFormatTsv;
  }
  if ([format isEqualToString:@"markdown"]) {
    return DSExportFormatMarkdown;
  }
  if ([format isEqualToString:@"html"]) {
    return DSExportFormatHtml;
  }
  return DSExportFormatText;
}

/// A CSV separator the locale chooses: a comma unless this locale writes
/// decimals with one, which is what Streamdown's `auto` does in the browser.
char16_t resolveSeparator(NSString *configured) {
  if ([configured isEqualToString:@";"]) {
    return u';';
  }
  if ([configured isEqualToString:@"\\t"] || [configured isEqualToString:@"\t"]) {
    return u'\t';
  }
  if (![configured isEqualToString:@"auto"]) {
    return u',';
  }
  return [NSLocale.currentLocale.decimalSeparator isEqualToString:@","] ? u';' : u',';
}

NSString *nameForFormat(NSString *format) {
  if ([format isEqualToString:@"csv"]) {
    return @"csv";
  }
  if ([format isEqualToString:@"tsv"]) {
    return @"tsv";
  }
  if ([format isEqualToString:@"markdown"]) {
    return @"md";
  }
  return @"txt";
}

} // namespace

@interface DownstreamMarkdownView () <RCTDownstreamMarkdownViewViewProtocol, DSLinkDelegate, DSControlDelegate>
- (void)scheduleTraceFrame;
- (void)applyRendering;
@end

@implementation DownstreamMarkdownView {
  DSDocumentView *_documentView;
  DSFileExporter *_exporter;
  DownstreamMarkdownShadowNode::ConcreteState::Shared _state;
  downstream::DocumentHandle _document;
  CGFloat _fontScale;
  BOOL _syntaxHighlighting;
  BOOL _selectable;
  uint32_t _controls;
  uint32_t _chrome;
  BOOL _codeBlockWrap;
  std::string _codeFilename;
  std::string _tableFilename;
  NSString *_csvSeparator;
  NSArray<NSString *> *_controlLabels;
  CADisplayLink *_traceDisplayLink;
  uint64_t _traceDocumentId;
  uint64_t _controlEpoch;
}

+ (ComponentDescriptorProvider)componentDescriptorProvider {
  return concreteComponentDescriptorProvider<DownstreamMarkdownViewComponentDescriptor>();
}

- (instancetype)initWithFrame:(CGRect)frame {
  if (self = [super initWithFrame:frame]) {
    static const auto defaultProps = std::make_shared<const DownstreamMarkdownViewProps>();
    _props = defaultProps;

    _syntaxHighlighting = YES;
    _selectable = YES;
    _controls = defaultProps->controls;
    _codeBlockWrap = NO;
    _codeFilename = "file";
    _tableFilename = "table";
    _csvSeparator = @",";
    _controlLabels = @[];
    _exporter = [DSFileExporter new];
    _documentView = [[DSDocumentView alloc] initWithFrame:self.bounds];
    _documentView.linkDelegate = self;
    _documentView.controlDelegate = self;
    _documentView.selectable = _selectable;
    _documentView.controls = _controls;
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
  NSMutableSet<NSNumber *> *toggles = [NSMutableSet new];
  for (const uint32_t ordinal : data.wrapToggles) {
    [toggles addObject:@(ordinal)];
  }
  if (data.document == _document && data.fontSizeMultiplier == _fontScale && data.chrome == _chrome &&
      [_documentView.codeWrapToggles isEqualToSet:toggles]) {
    return;
  }
  _document = data.document;
  _fontScale = data.fontSizeMultiplier;
  _chrome = data.chrome;
  _documentView.codeWrapToggles = toggles;
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
  _documentView.chrome = _chrome;
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
    ++_controlEpoch;
    [_documentView resetTableScrollPositions];
  }
  if (newProps.selectable != _selectable) {
    _selectable = newProps.selectable;
    _documentView.selectable = _selectable;
  }
  if (newProps.codeBlockWrap != _codeBlockWrap) {
    _codeBlockWrap = newProps.codeBlockWrap;
    _documentView.codeBlockWrap = _codeBlockWrap;
  }
  if (newProps.controls != _controls) {
    _controls = newProps.controls;
    _documentView.controls = _controls;
  }
  if (newProps.codeFilename != previous.codeFilename) {
    _codeFilename = newProps.codeFilename;
  }
  if (newProps.tableFilename != previous.tableFilename) {
    _tableFilename = newProps.tableFilename;
  }
  if (newProps.csvSeparator != previous.csvSeparator) {
    _csvSeparator = newProps.csvSeparator.empty() ? @"," : @(newProps.csvSeparator.c_str());
  }
  if (newProps.controlLabels != previous.controlLabels) {
    NSMutableArray<NSString *> *labels = [NSMutableArray arrayWithCapacity:newProps.controlLabels.size()];
    for (const std::string &label : newProps.controlLabels) {
      [labels addObject:@(label.c_str())];
    }
    _controlLabels = labels;
    [_documentView setControlLabels:_controlLabels];
  }
  // The chrome itself is part of the state, because it is a height and the
  // shadow node is what measures; a prop change reaches it through a state
  // update on the next layout.
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
  ++_controlEpoch;
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

#pragma mark - Controls

/// The event emitter, or null when the view has been recycled. A control that
/// outlives its view must report nothing: there is no JavaScript left to
/// report it to.
- (const DownstreamMarkdownViewEventEmitter *)emitter {
  return _eventEmitter
      ? std::static_pointer_cast<const DownstreamMarkdownViewEventEmitter>(_eventEmitter).get()
      : nullptr;
}

- (void)emitControlWithBlock:(NSString *)block
                      action:(NSString *)action
                      format:(NSString *)format
                    language:(NSString *)language
                    filename:(NSString *)filename
                     wrapped:(BOOL)wrapped {
  const auto *emitter = [self emitter];
  if (emitter == nullptr) {
    return;
  }
  // The press carries the generation, so a document that has been replaced
  // since drops it rather than reporting it to the new one's handlers.
  const int generation = _state
      ? static_cast<int>(std::static_pointer_cast<const DownstreamMarkdownViewProps>(_props)->generation)
      : 0;
  emitter->onControlAction({
      .block = std::string(block.UTF8String),
      .action = std::string(action.UTF8String),
      .format = std::string(format.UTF8String),
      .language = std::string(language.UTF8String),
      .filename = std::string(filename.UTF8String),
      .wrapped = static_cast<bool>(wrapped),
      .generation = generation,
  });
}

- (void)emitControlFailureFor:(NSString *)block action:(NSString *)action message:(NSString *)message {
  if (!_state || _eventEmitter == nullptr) {
    return;
  }
  // The block and action travel as fields, so routing a failure to its own
  // onError does not depend on how the message reads and an app is told which
  // control failed. The message still names it, for a log line.
  std::static_pointer_cast<const DownstreamMarkdownViewEventEmitter>(_eventEmitter)
      ->onMarkdownError({
          .code = "control_failed",
          .message = std::string([NSString stringWithFormat:@"%@ %@ failed: %@", block, action, message]
                                      .UTF8String),
          .generation = static_cast<int>(std::static_pointer_cast<const DownstreamMarkdownViewProps>(_props)->generation),
          .sequence = 0,
          .recoverable = true,
          .block = std::string(block.UTF8String),
          .action = std::string(action.UTF8String),
      });
}

- (void)didPressControlOfKind:(DSBlockKind)kind
                   blockIndex:(NSInteger)blockIndex
                   blockCount:(uint32_t)blockCount
                     language:(NSString *)language
                      action:(NSString *)action
                      format:(NSString *)format
                    filename:(NSString *)filename {
  if (!_document) {
    return;
  }
  const bool isCode = kind == DSBlockKindCode;
  // The kind says which block type the range holds and the index says where it
  // starts, so the range is read from the index and the kind only chooses the
  // format. A range whose first block is not of that kind is a caller bug, and
  // reporting it beats copying whatever happens to live at those indexes.
  if (blockIndex < 0 || static_cast<size_t>(blockIndex) >= _document->blocks.size()) {
    [self emitControlFailureFor:isCode ? @"code" : @"table" action:action message:@"The block is gone."];
    return;
  }
  const size_t from = static_cast<size_t>(blockIndex);
  const size_t to = std::min(from + blockCount, _document->blocks.size());
  if (from >= to) {
    return;
  }
  if ((_document->blocks[from].kind == BlockKind::CodeBlock) != isCode) {
    [self emitControlFailureFor:isCode ? @"code" : @"table"
                         action:action
                        message:@"The control and the block it names disagree."];
    return;
  }
  const bool download = [action isEqualToString:@"download"];
  const uint64_t copyEpoch = download ? 0 : ++copyRequestEpoch;
  const NSInteger clipboardChangeCount = download ? 0 : UIPasteboard.generalPasteboard.changeCount;
  const char16_t separator = download ? resolveSeparator(_csvSeparator) : kDefaultCsvSeparator;
  // Code always copies the text it shows; a table is copied in the format
  // its menu chose.
  const ExportFormat exportFormat = isCode ? ExportFormat::Text : static_cast<ExportFormat>(formatFor(format));

  // Retain the immutable snapshot, not the view. Streaming can publish newer
  // snapshots while this worker serializes the settled range that was pressed.
  const DocumentHandle document = _document;
  const uint64_t epoch = _controlEpoch;
  const std::string codeFilename = _codeFilename;
  const std::string tableFilename = _tableFilename;
  __weak DownstreamMarkdownView *weakSelf = self;
  void (^finish)(NSString *, NSString *, NSData *, NSString *, NSString *) =
      ^(NSString *text, NSString *html, NSData *data, NSString *name, NSString *failure) {
    DownstreamMarkdownView *view = weakSelf;
    if (view == nil || view->_controlEpoch != epoch || !view->_state || !view->_eventEmitter ||
        (!download && (copyEpoch != copyRequestEpoch ||
                       UIPasteboard.generalPasteboard.changeCount != clipboardChangeCount))) {
      return;
    }
    if (failure != nil) {
      [view emitControlFailureFor:isCode ? @"code" : @"table" action:action message:failure];
      return;
    }
    if (!download) {
      if (isCode) {
        UIPasteboard.generalPasteboard.string = text;
      } else {
        [UIPasteboard.generalPasteboard setItems:@[
          @{@"public.utf8-plain-text" : text, @"public.html" : html},
        ]];
      }
      [view emitControlWithBlock:isCode ? @"code" : @"table"
                          action:action
                          format:format
                        language:language
                        filename:@""
                         wrapped:NO];
      return;
    }
    [view->_exporter saveData:data
                    fileName:name
               shouldPresent:^BOOL {
      DownstreamMarkdownView *current = weakSelf;
      return current != nil && current->_controlEpoch == epoch && current->_state && current->_eventEmitter;
    }
                   completion:^(NSURL *url, NSError *error) {
      DownstreamMarkdownView *current = weakSelf;
      if (current == nil || current->_controlEpoch != epoch || !current->_state || !current->_eventEmitter) {
        return;
      }
      if (error != nil) {
        [current emitControlFailureFor:isCode ? @"code" : @"table"
                                action:@"download"
                               message:error.localizedDescription];
      } else if (url != nil) {
        [current emitControlWithBlock:isCode ? @"code" : @"table"
                               action:action
                               format:format
                             language:language
                             filename:name
                              wrapped:NO];
      }
    }];
  };

  size_t estimatedBytes = 0;
  for (size_t index = from; index < to && estimatedBytes <= kInlineExportBytes; ++index) {
    const Block &block = document->blocks[index];
    if (block.text.size() > kInlineExportBytes / sizeof(char16_t) ||
        block.cells.size() > kInlineExportBytes / 32) {
      estimatedBytes = kInlineExportBytes + 1;
      break;
    }
    estimatedBytes += block.text.size() * sizeof(char16_t) + block.cells.size() * 32 + 64;
  }
  const BOOL inlineExport = estimatedBytes <= kInlineExportBytes;
  void (^serialize)(void) = ^{
    @autoreleasepool {
      NSString *text = nil;
      NSString *html = nil;
      NSData *data = nil;
      NSString *name = @"";
      NSString *failure = nil;
      try {
        text = toNSString(exportBlocks(*document, from, to, exportFormat, separator));
        if (download) {
          name = isCode ? toNSString(downloadName(document->blocks[from].info, document->blocks[from].language,
                                                 codeFilename, "file"))
                        : toNSString(tableDownloadName(tableFilename, std::string_view(nameForFormat(format).UTF8String)));
          NSMutableData *bytes = [NSMutableData data];
          // Excel on Windows needs the BOM to identify UTF-8 CSV text.
          if ([format isEqualToString:@"csv"]) {
            const uint8_t bom[] = {0xEF, 0xBB, 0xBF};
            [bytes appendBytes:bom length:sizeof(bom)];
          }
          [bytes appendData:[text dataUsingEncoding:NSUTF8StringEncoding]];
          data = [bytes copy];
        } else if (!isCode) {
          html = toNSString(exportBlocks(*document, from, to, ExportFormat::Html, separator));
        }
      } catch (const std::exception &error) {
        failure = toNSString(std::string(error.what()));
      } catch (...) {
        failure = @"Could not serialize the block.";
      }
      if (inlineExport) {
        finish(text, html, data, name, failure);
      } else {
        dispatch_async(dispatch_get_main_queue(), ^{
          finish(text, html, data, name, failure);
        });
      }
    }
  };
  if (inlineExport) {
    serialize();
  } else {
    WorkerPool::shared().post([serialize] { serialize(); });
  }
}

- (void)didToggleWrapAtOrdinal:(uint32_t)ordinal language:(NSString *)language wrapped:(BOOL)wrapped {
  const auto *emitter = [self emitter];
  if (emitter == nullptr || !_state) {
    return;
  }
  // The height changes, and only Yoga and the chat list around the message can
  // know that, so the change goes back through a Fabric state update rather
  // than a React render: no parse, no JavaScript work, no re-measure of the
  // text, which is laid out again only for the block that changed. The state
  // carries the prop's own wrap setting, which is the half of the decision the
  // toggled set completes, so measurement resolves the same way afterwards.
  DownstreamMarkdownState next = _state->getData().toggled(ordinal, _controls);
  _state->updateState(std::move(next));
  [self emitControlWithBlock:@"code" action:@"wrap" format:@"" language:language filename:@"" wrapped:wrapped];
}

- (void)didFailControlOfKind:(DSBlockKind)kind action:(NSString *)action message:(NSString *)message {
  [self emitControlFailureFor:kind == DSBlockKindCode ? @"code" : @"table"
                        action:action
                       message:message];
}


@end

Class<RCTComponentViewProtocol> DownstreamMarkdownViewCls(void) {
  return DownstreamMarkdownView.class;
}
