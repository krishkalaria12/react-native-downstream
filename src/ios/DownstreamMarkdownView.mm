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

#include <algorithm>
#include <string>
#include <vector>

using namespace facebook::react;
using namespace downstream;

namespace {

/// A segment whose text is under this size is serialized on the main thread,
/// which is well inside a frame. A bigger one goes to the worker pool, so a
/// table with thousands of cells cannot cost a reader a dropped frame.
constexpr size_t kInlineExportBytes = 64 * 1024;

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
  emitter->onControlAction({
      .block = std::string(block.UTF8String),
      .action = std::string(action.UTF8String),
      .format = std::string(format.UTF8String),
      .language = std::string(language.UTF8String),
      .filename = std::string(filename.UTF8String),
      .wrapped = static_cast<bool>(wrapped),
  });
}

- (void)emitControlFailureFor:(NSString *)block action:(NSString *)action message:(NSString *)message {
  if (!_state || _eventEmitter == nullptr) {
    return;
  }
  // The message names the control, so a per-control onError and the view's own
  // onError can both find the failure without a second event.
  std::static_pointer_cast<const DownstreamMarkdownViewEventEmitter>(_eventEmitter)
      ->onMarkdownError({
          .code = "control_failed",
          .message = std::string([NSString stringWithFormat:@"%@ %@ failed: %@", block, action, message]
                                      .UTF8String),
          .generation = static_cast<int>(std::static_pointer_cast<const DownstreamMarkdownViewProps>(_props)->generation),
          .sequence = 0,
          .recoverable = true,
      });
}

- (void)didPressControlOfKind:(NSInteger)blockIndex
                   blockCount:(uint32_t)blockCount
                     language:(NSString *)language
                      action:(NSString *)action
                      format:(NSString *)format
                    filename:(NSString *)filename {
  if (!_document) {
    return;
  }
  const size_t from = static_cast<size_t>(blockIndex);
  const size_t to = std::min(from + blockCount, _document->blocks.size());
  if (from >= to) {
    return;
  }
  const bool isCode = blockIndex == DSBlockKindCode;
  const bool download = [action isEqualToString:@"download"];
  const char16_t separator = download ? resolveSeparator(_csvSeparator) : kDefaultCsvSeparator;
  // Code always copies the text it shows; a table is copied in the format
  // its menu chose.
  const ExportFormat exportFormat = isCode ? ExportFormat::Text : static_cast<ExportFormat>(formatFor(format));

  // The bytes a reader gets for a settled block cannot change, so a press reads
  // the snapshot on screen and no later snapshot can make that copy stale.
  const std::u16string text = exportBlocks(*_document, from, to, exportFormat, separator);
  NSString *name = @"";
  if (download) {
    name = isCode ? toNSString(downloadName(_document->blocks[from].info, _document->blocks[from].language,
                                            _codeFilename, "file"))
                  : toNSString(tableDownloadName(_tableFilename, std::string_view(nameForFormat(format).UTF8String)));
  }

  if (!download) {
    if (isCode) {
      UIPasteboard.generalPasteboard.string = toNSString(text);
    } else {
      // A table also goes on the clipboard as HTML, so a paste into Notes, Docs,
      // Sheets, or Mail lands as a table rather than as text.
      const std::u16string html = exportBlocks(*_document, from, to, ExportFormat::Html, separator);
      [UIPasteboard.generalPasteboard setItems:@[
        @{@"public.utf8-plain-text" : toNSString(text), @"public.html" : toNSString(html)},
      ]];
    }
    [self emitControlWithBlock:isCode ? @"code" : @"table"
                        action:action
                        format:format
                      language:language
                      filename:@""
                       wrapped:NO];
    return;
  }

  // Excel on Windows reads a CSV without a byte-order mark as the system
  // codepage, which mangles anything outside it, so a CSV gets one.
  NSMutableData *data = [NSMutableData data];
  if ([format isEqualToString:@"csv"]) {
    const uint8_t bom[] = {0xEF, 0xBB, 0xBF};
    [data appendBytes:bom length:sizeof(bom)];
  }
  [data appendData:[toNSString(text) dataUsingEncoding:NSUTF8StringEncoding]];
  [_exporter saveData:data
             fileName:name
            completion:^(NSURL *url, NSError *error) {
              if (error != nil) {
                [self emitControlFailureFor:isCode ? @"code" : @"table" action:@"download" message:error.localizedDescription];
                return;
              }
              // A cancelled dialog reports nothing: it is neither a success nor
              // a failure.
              if (url == nil) {
                return;
              }
              [self emitControlWithBlock:isCode ? @"code" : @"table"
                                  action:action
                                  format:format
                                language:language
                                filename:name
                                 wrapped:NO];
            }];
}

- (void)didToggleWrapAtOrdinal:(uint32_t)ordinal language:(NSString *)language wrapped:(BOOL)wrapped {
  const auto *emitter = [self emitter];
  if (emitter == nullptr || !_state) {
    return;
  }
  // The height changes, and only Yoga and the chat list around the message can
  // know that, so the change goes back through a Fabric state update rather
  // than a React render: no parse, no JavaScript work, no re-measure of the
  // text, which is laid out again only for the block that changed.
  DownstreamMarkdownState next = _state->getData().toggled(ordinal, _controls, _codeBlockWrap);
  _state->updateState(std::move(next));
  [self emitControlWithBlock:@"code" action:@"wrap" format:@"" language:language filename:@"" wrapped:wrapped];
}

- (void)didFailControlOfKind:(NSInteger)blockIndex action:(NSString *)action message:(NSString *)message {
  [self emitControlFailureFor:blockIndex == DSBlockKindCode ? @"code" : @"table"
                       action:action
                      message:message];
}

@end

Class<RCTComponentViewProtocol> DownstreamMarkdownViewCls(void) {
  return DownstreamMarkdownView.class;
}
