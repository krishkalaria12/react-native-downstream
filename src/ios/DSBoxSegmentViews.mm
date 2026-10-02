// The image and math segment views.
//
// Separate from DSSegmentViews.mm because they are the two segments that own
// something other than text: an image owns a request and a button, and a formula
// owns a display list and a scroll view. Everything they share with the text and
// code segments — the control button, the control labels, the symbol set — comes
// from there through the header, so the two files cannot disagree about what a
// button looks like.

#import "DSSegmentViews.h"

#import "DSFileExporter.h"
#import "DSImageLoader.h"
#import "DSMathFonts.h"
#import "DSRenderedDocument.h"

#include "Controls.h"
#include "Export.h"
#include "ImageLoaderRegistry.h"
#include "ImageSizeCache.h"
#include "math/MathSpeech.h"

#include <algorithm>
#include <cmath>
#include <string>

using namespace downstream;

namespace {

/// The download glyph, shared with the code and table downloads.
NSString *const kDownloadSymbol = @"arrow.down.to.line";

/// The button's side, which is the code and table controls' size too.
constexpr CGFloat kControlGlyph = 28;

/// Indexes into the label array `src/controls.ts` sends, in its order. The four
/// image keys are appended there after the table's seventeen, and the same
/// offsets are in `DSSegmentViews.mm`'s own enum, so the two files name the same
/// positions rather than each writing a number.
typedef NS_ENUM(NSInteger, DSImageControlLabel) {
  kImageNotAvailableLabel = 17,
  kImageDownloadLabel = 18,
  kImageLabel = 19,
  kImageBlockedLabel = 20,
};

} // namespace

#pragma mark - One image

@interface DSImageSegmentView ()
/// Saves one image, which its box's download button asks for.
- (void)downloadImageAt:(uint32_t)ordinal;
@end

/// One image's own subview: a picture, the placeholder, or the fallback line.
///
/// A subview per image rather than one view drawing all of them, because each has
/// its own request, its own press, and its own download button — and a chat list
/// recycles segments, so one subview per image is what lets a recycled view
/// release only the requests it no longer wants.
@interface DSImageBoxView : UIView
@property (nonatomic) uint32_t ordinal;
@property (nonatomic, copy) NSString *url;
@property (nonatomic, copy) NSString *alt;
@property (nonatomic, copy) NSString *title;
@property (nonatomic) BOOL loadable;
@property (nonatomic) BOOL settled;
@property (nonatomic) BOOL isLink;
@property (nonatomic) int32_t linkIndex;
@property (nonatomic) uint32_t controls;
@property (nonatomic) BOOL controlsEnabled;
@property (nonatomic, copy) NSString *downloadLabel;
@property (nonatomic, copy) NSString *notAvailableLabel;
@property (nonatomic, copy) NSString *blockedLabel;
@property (nonatomic, copy) NSString *imageLabel;
@property (nonatomic, strong, nullable) UIColor *placeholderColor;
@property (nonatomic, strong, nullable) UIColor *errorColor;
@property (nonatomic, strong, nullable) UIImageView *picture;
@property (nonatomic, strong, nullable) UIView *placeholder;
@property (nonatomic, strong, nullable) UILabel *fallback;
@property (nonatomic, strong, nullable) UIButton *download;
@property (nonatomic, strong, nullable) DSImageLoad *load;
@property (nonatomic, weak, nullable) DSImageSegmentView *owner;
/// The segment's own metrics, which the box draws from and never decides.
@property (nonatomic) float borderRadius;
@end

@implementation DSImageBoxView

- (void)dealloc {
  // A recycled box releases the request it holds. The loader keeps the request
  // alive while another view wants it, so this releases an *interest* rather than
  // the request, and scrolling a message off screen is a cancel.
  DSImageLoad *load = _load;
  _load = nil;
  if (load != nil) {
    [[DSImageLoader sharedLoaderWithReactLoader:nil] cancelLoad:load];
  }
}

- (void)buildForSize:(CGSize)size {
  _picture = [[UIImageView alloc] initWithFrame:CGRectMake(0, 0, size.width, size.height)];
  _picture.contentMode = UIViewContentModeScaleAspectFit;
  _picture.clipsToBounds = YES;
  _picture.hidden = YES;
  [self addSubview:_picture];

  _placeholder = [[UIView alloc] initWithFrame:CGRectMake(0, 0, size.width, size.height)];
  _placeholder.backgroundColor = self.placeholderColor;
  _placeholder.hidden = YES;
  [self addSubview:_placeholder];

  _fallback = [[UILabel alloc] initWithFrame:CGRectMake(0, 0, size.width, size.height)];
  // One line, because the box the engine reserves for a fallback is one line tall.
  _fallback.numberOfLines = 1;
  _fallback.lineBreakMode = NSLineBreakByTruncatingTail;
  _fallback.textColor = self.errorColor ?: UIColor.secondaryLabelColor;
  _fallback.font = [UIFont systemFontOfSize:15];
  _fallback.hidden = YES;
  [self addSubview:_fallback];

  _download = [UIButton buttonWithType:UIButtonTypeSystem];
  UIImageSymbolConfiguration *configuration =
      [UIImageSymbolConfiguration configurationWithPointSize:kControlGlyph * 0.5
                                                      weight:UIImageSymbolWeightRegular];
  [_download setImage:[UIImage systemImageNamed:kDownloadSymbol withConfiguration:configuration]
             forState:UIControlStateNormal];
  _download.tintColor = UIColor.labelColor;
  // A round backing, because the button sits on the picture and a bare glyph
  // disappears on a dark or busy corner. The system background follows the
  // appearance, so it is light on light and dark on dark.
  _download.backgroundColor = [UIColor.systemBackgroundColor colorWithAlphaComponent:0.85];
  _download.layer.cornerRadius = kControlGlyph / 2;
  _download.clipsToBounds = YES;
  _download.accessibilityLabel = self.downloadLabel;
  _download.frame = CGRectMake(0, 0, kControlGlyph, kControlGlyph);
  [_download.widthAnchor constraintEqualToConstant:kControlGlyph].active = YES;
  [_download.heightAnchor constraintEqualToConstant:kControlGlyph].active = YES;
  _download.hidden = YES;
  [_download addTarget:self action:@selector(downloadPressed) forControlEvents:UIControlEventTouchUpInside];
  [self addSubview:_download];

  self.layer.masksToBounds = YES;
  self.isAccessibilityElement = YES;
}

/// The placeholder breathes, unless the reader has asked for less motion.
///
/// Reduce Motion holds it still rather than removing it: a reader who turned
/// animation off still needs to see that something is loading, and a box that
/// appears without ever having pulsed is a box that looks broken.
- (void)startPulse {
  if (self.placeholder == nil || self.placeholder.hidden || UIAccessibilityIsReduceMotionEnabled()) {
    return;
  }
  CABasicAnimation *animation = [CABasicAnimation animationWithKeyPath:@"opacity"];
  animation.fromValue = @0.45;
  animation.toValue = @1.0;
  animation.duration = 0.9;
  animation.autoreverses = YES;
  animation.repeatCount = HUGE_VALF;
  [self.placeholder.layer addAnimation:animation forKey:@"ds.placeholder"];
}

- (void)stopPulse {
  [self.placeholder.layer removeAnimationForKey:@"ds.placeholder"];
}

- (void)applyState:(ImageState)state {
  self.picture.hidden = state != ImageState::Loaded;
  const BOOL showsFallback = state == ImageState::Failed || state == ImageState::Blocked ||
      state == ImageState::Disabled;
  self.fallback.hidden = !showsFallback;
  self.download.hidden = state != ImageState::Loaded || !self.controlsEnabled ||
      (self.controls & ControlBits::ImageDownload) == 0;
  const BOOL showsPlaceholder = state == ImageState::Pending || state == ImageState::Loading;
  self.placeholder.hidden = !showsPlaceholder;
  if (showsPlaceholder) {
    [self startPulse];
  } else {
    [self stopPulse];
  }
  self.placeholder.backgroundColor = self.placeholderColor;
  self.placeholder.layer.cornerRadius = self.borderRadius;
  self.picture.layer.cornerRadius = self.borderRadius;

  // The fallback says what happened, and leads with the writer's own words for
  // the picture: a reader who cannot see it is better served by the alt text than
  // by a status line alone, and by both when both exist.
  NSString *status = nil;
  switch (state) {
    case ImageState::Failed:
      status = self.notAvailableLabel;
      break;
    case ImageState::Blocked:
      status = self.blockedLabel;
      break;
    case ImageState::Disabled:
      status = self.alt;
      break;
    case ImageState::Pending:
    case ImageState::Loading:
    case ImageState::Loaded:
      break;
  }
  if (status != nil) {
    self.fallback.text = self.alt.length > 0 && state != ImageState::Disabled
        ? [NSString stringWithFormat:@"%@ · %@", self.alt, status]
        : status;
  } else {
    self.fallback.text = self.alt;
  }
  [self updateAccessibilityFor:state];
}

/// One element per image: the image trait (or link, when the picture is a link's
/// hit area), the alt text as the label, and the state as the value so a screen
/// reader says "loading" rather than nothing at all.
- (void)updateAccessibilityFor:(ImageState)state {
  self.accessibilityLabel = self.alt.length > 0 ? self.alt : self.imageLabel;
  NSString *value = nil;
  switch (state) {
    case ImageState::Pending:
    case ImageState::Loading:
      value = self.downloadLabel;
      break;
    case ImageState::Failed:
    case ImageState::Blocked:
    case ImageState::Disabled:
      value = self.fallback.text;
      break;
    case ImageState::Loaded:
      break;
  }
  self.accessibilityValue = value;
  if (state == ImageState::Disabled) {
    // Turned off is not an image, so it is not announced as one.
    self.accessibilityTraits = UIAccessibilityTraitStaticText;
  } else if (self.isLink) {
    self.accessibilityTraits = UIAccessibilityTraitLink | UIAccessibilityTraitImage;
  } else {
    self.accessibilityTraits = UIAccessibilityTraitImage;
  }
}

- (void)layoutSubviews {
  [super layoutSubviews];
  const CGRect bounds = self.bounds;
  self.picture.frame = bounds;
  self.placeholder.frame = bounds;
  self.fallback.frame = bounds;
  // Bottom-trailing, as Streamdown draws it: over the picture, out of the way of
  // a caption, and large enough to hit.
  self.download.frame = CGRectMake(std::max<CGFloat>(0, CGRectGetWidth(bounds) - kControlGlyph - 4),
                                   std::max<CGFloat>(0, CGRectGetHeight(bounds) - kControlGlyph - 4), kControlGlyph,
                                   kControlGlyph);
}

- (void)downloadPressed {
  DSImageSegmentView *owner = self.owner;
  [owner downloadImageAt:self.ordinal];
}

- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
  if (!self.download.hidden) {
    // A press that lands on the button is the button's, and the picture's press
    // must not also fire: a reader who wanted to save a chart did not mean to
    // open it.
    const CGPoint point = touches.anyObject ? [touches.anyObject locationInView:self] : CGPointZero;
    if (CGRectContainsPoint(self.download.frame, point)) {
      [super touchesEnded:touches withEvent:event];
      return;
    }
  }
  if (self.picture.hidden) {
    [super touchesEnded:touches withEvent:event];
    return;
  }
  if (self.isLink) {
    // A linked image reaches `onLinkPress` with the link's own range, so an app
    // that already handles links needs no image-specific case.
    [self.owner.linkDelegate didPressLinkAtIndex:self.linkIndex];
  } else {
    [self.owner.imageDelegate didPressImageWithURL:self.url
                                              alt:self.alt
                                            title:self.title
                                         ordinal:self.ordinal];
  }
}

@end

#pragma mark - One image block

@interface DSImageSegmentView ()
@property (nonatomic, nullable) DSImageSegment *segment;
@property (nonatomic) ImageStateLayout state;
@property (nonatomic) NSArray<DSImageBoxView *> *boxes;
@property (nonatomic, copy) NSArray<NSString *> *labels;
@property (nonatomic, strong, nullable) DSFileExporter *exporter;
@end

@implementation DSImageSegmentView

- (instancetype)initWithFrame:(CGRect)frame {
  if (!(self = [super initWithFrame:frame])) {
    return nil;
  }
  _boxes = @[];
  _labels = @[];
  return self;
}

- (NSString *)label:(NSInteger)index {
  return (NSUInteger)index < _labels.count ? _labels[(NSUInteger)index] : @"";
}

- (void)setSegment:(DSImageSegment *)segment {
  _segment = segment;
  NSMutableArray<DSImageBoxView *> *boxes = [NSMutableArray arrayWithCapacity:segment.images.size()];
  for (NSUInteger index = 0; index < segment.images.size(); index++) {
    NSString *url = index < segment.urls.size() ? @(segment.urls[index].c_str()) : @"";
    // A box for the same picture is kept across snapshots, so a streaming message
    // does not cancel and restart a load every time a token arrives, and a loaded
    // picture does not flash back to its placeholder.
    DSImageBoxView *box = nil;
    if (index < _boxes.count && _boxes[index].ordinal == segment.images[index] &&
        [_boxes[index].url isEqualToString:url]) {
      box = _boxes[index];
    } else {
      box = [DSImageBoxView new];
      [box buildForSize:CGSizeZero];
    }
    box.ordinal = segment.images[index];
    box.url = url;
    box.alt = index < segment.altTexts.size() ? @(segment.altTexts[index].c_str()) : @"";
    box.title = index < segment.titles.size() ? @(segment.titles[index].c_str()) : @"";
    box.linkIndex = index < segment.links.size() ? segment.links[index] : -1;
    box.isLink = box.linkIndex >= 0;
    box.loadable = url.length > 0;
    box.owner = self;
    box.placeholderColor = segment.placeholderColor;
    box.borderRadius = segment.metrics.borderRadius;
    box.downloadLabel = [self label:kImageDownloadLabel];
    box.notAvailableLabel = [self label:kImageNotAvailableLabel];
    box.blockedLabel = [self label:kImageBlockedLabel];
    box.imageLabel = [self label:kImageLabel];
    box.settled = self.settled;
    box.controls = self.controls;
    box.controlsEnabled = self.controlsEnabled;
    [boxes addObject:box];
    if (box.superview != self) {
      [self addSubview:box];
    }
  }
  for (DSImageBoxView *old in _boxes) {
    if (![boxes containsObject:old]) {
      [old removeFromSuperview];
    }
  }
  _boxes = boxes;
  [self setNeedsLayout];
}

- (void)setControlLabels:(NSArray<NSString *> *)labels {
  _labels = labels ?: @[];
  for (DSImageBoxView *box in _boxes) {
    box.downloadLabel = [self label:kImageDownloadLabel];
    box.notAvailableLabel = [self label:kImageNotAvailableLabel];
    box.blockedLabel = [self label:kImageBlockedLabel];
    box.imageLabel = [self label:kImageLabel];
    box.download.accessibilityLabel = box.downloadLabel;
  }
}

- (void)setImageState:(const ImageStateLayout &)state {
  _state = state;
  [self setNeedsLayout];
}

- (void)setControls:(uint32_t)controls {
  _controls = controls;
  for (DSImageBoxView *box in _boxes) {
    box.controls = controls;
  }
  [self setNeedsLayout];
}

- (void)setControlsEnabled:(BOOL)controlsEnabled {
  _controlsEnabled = controlsEnabled;
  for (DSImageBoxView *box in _boxes) {
    box.controlsEnabled = controlsEnabled;
  }
  [self setNeedsLayout];
}

- (void)setSettled:(BOOL)settled {
  _settled = settled;
  for (DSImageBoxView *box in _boxes) {
    box.settled = settled;
  }
  [self setNeedsLayout];
}

- (void)setScreenScale:(CGFloat)screenScale {
  _screenScale = screenScale;
  [self setNeedsLayout];
}

- (void)layoutSubviews {
  [super layoutSubviews];
  DSImageSegment *segment = self.segment;
  if (segment == nil) {
    return;
  }
  const CGFloat column = std::max<CGFloat>(0, CGRectGetWidth(self.bounds) - segment.contentLeading);
  CGFloat y = 0;
  for (NSUInteger index = 0; index < self->_boxes.count; index++) {
    DSImageBoxView *box = self->_boxes[index];
    // The one box rule, called with the same arguments the measurement used. A
    // view that decided its own height would be free to disagree with the frame
    // it was given, and a picture drawn in the wrong box is the most visible bug
    // this library could have.
    const ImageBox measured = [segment boxAtIndex:index column:column answers:self.state];
    // The ivar, not the property: the property returns a copy, and a pointer into
    // a temporary copy dangles before the next line reads it.
    const ImageEntry *entry = _state.find(box.ordinal);
    const ImageState state = entry != nullptr
        ? entry->state
        : (_state.enabled ? ImageState::Pending : ImageState::Disabled);
    box.frame = CGRectMake(segment.contentLeading, y, measured.width, measured.height);
    [box applyState:state];
    [self startLoadFor:box at:state size:measured column:column];
    y += measured.height + segment.metrics.marginTop;
  }
}

- (void)startLoadFor:(DSImageBoxView *)box
                 at:(ImageState)state
               size:(ImageBox)boxSize
             column:(CGFloat)column {
  // A `Loaded` answer with no picture is a box that was built after the load
  // finished: a recycled view, or a message scrolled back into view. The platform
  // loader's own cache answers it without a request.
  const BOOL wanted = state == ImageState::Loading || (state == ImageState::Loaded && box.picture.image == nil);
  if (!wanted || box.load != nil || !box.loadable || box.url.length == 0 || !(column > 0)) {
    return;
  }
  NSURL *url = [NSURL URLWithString:box.url];
  if (url == nil) {
    return;
  }
  const CGFloat scale = self.screenScale > 0 ? self.screenScale : UIScreen.mainScreen.scale;
  // The decode bound is the column and the theme's height cap, not the box: the
  // box is a placeholder until the picture's size is known, and decoding against
  // it would make every picture placeholder-sized.
  const CGFloat boundHeight = self.segment.metrics.maxHeight > 0 ? self.segment.metrics.maxHeight : column;
  // The platform's own pipeline, read from the context container the way React
  // Native's `ImageManager` reads it.
  DSImageLoader *loader = [DSImageLoader
      sharedLoaderWithReactLoader:(__bridge RCTImageLoader *)ImageLoaderRegistry::resolve()];
  __weak DSImageSegmentView *weakSelf = self;
  __weak DSImageBoxView *weakBox = box;
  const std::string key = box.url.UTF8String ?: "";
  box.load = [loader loadURL:url
                        size:CGSizeMake(column * scale, boundHeight * scale)
                       scale:scale
                    callback:^(DSImageLoad *load, DSImageLoadResult *result) {
                      DSImageSegmentView *strongSelf = weakSelf;
                      DSImageBoxView *strongBox = weakBox;
                      if (strongBox != nil) {
                        strongBox.load = nil;
                      }
                      if (strongSelf == nil) {
                        return;
                      }
                      // Copied, because the entry is overwritten in place below.
                      const CachedImageSize *found = ImageSizeCache::find(key);
                      const bool hadAnswer = found != nullptr;
                      const CachedImageSize before = hadAnswer ? *found : CachedImageSize{};
                      if (result == nil || result.status == DSImageLoadStatusFailed || result.image == nil) {
                        // Recorded, so the next measurement draws the alt text and a
                        // scrolled-back message does not refetch a picture that 404s.
                        ImageSizeCache::record(key, {ImageState::Failed, 0, 0});
                        [strongSelf.imageDelegate imagesDidChange];
                        return;
                      }
                      strongBox.picture.image = result.image;
                      const CachedImageSize size{ImageState::Loaded, static_cast<float>(result.pixelSize.width),
                                                 static_cast<float>(result.pixelSize.height)};
                      ImageSizeCache::record(key, size);
                      if (!hadAnswer || before.state != size.state || before.pixelWidth != size.pixelWidth ||
                          before.pixelHeight != size.pixelHeight) {
                        // The node measures again from the cache; a picture whose
                        // answer was already known needs no new layout.
                        [strongSelf.imageDelegate imagesDidChange];
                      } else {
                        [strongSelf setNeedsLayout];
                      }
                    }];
}

/// Saves one image, under Streamdown's own naming rules.
///
/// The bytes are fetched once, here, rather than kept from the load: the
/// platform's loader hands back a decoded image and keeps its own cache, and the
/// bytes are not part of that contract. So a save costs one request — which is
/// the request the web version cannot make, where a CORS failure leaves a reader
/// with no way to save a picture at all.
- (void)downloadImageAt:(uint32_t)ordinal {
  DSImageSegment *segment = self.segment;
  if (segment == nil || !self.controlsEnabled || (self.controls & ControlBits::ImageDownload) == 0) {
    return;
  }
  NSURL *destination = nil;
  // The alt text names the file the way a writer's caption names it, and the
  // extension comes from the URL, the MIME type, or `png`, in that order.
  NSString *caption = @"";
  for (DSImageBoxView *box in _boxes) {
    if (box.ordinal != ordinal) {
      continue;
    }
    destination = [NSURL URLWithString:box.url];
    caption = box.alt ?: @"";
    break;
  }
  if (destination == nil) {
    return;
  }
  if (self.exporter == nil) {
    self.exporter = [DSFileExporter new];
  }
  __weak DSImageSegmentView *weakSelf = self;
  DSDownloadImageBytes(destination, ^(NSData *data, NSString *mimeType) {
    DSImageSegmentView *strong = weakSelf;
    if (strong == nil) {
      return;
    }
    if (data == nil) {
      [strong.controlDelegate didFailControlOfKind:DSBlockKindImage
                                           action:@"download"
                                          message:@"The image could not be fetched."];
      return;
    }
    // Streamdown's three naming rules, in the engine, so both platforms and every
    // release of this library produce the same name for the same picture.
    const std::string name =
        imageDownloadName(destination.absoluteString.UTF8String ?: "", caption.UTF8String ?: "",
                          mimeType.UTF8String ?: "", "image");
    [strong.exporter saveData:data
                     fileName:[NSString stringWithUTF8String:name.c_str()]
                shouldPresent:^BOOL {
                  return strong != nil;
                }
                  completion:^(NSURL *url, NSError *error) {
                    if (error != nil) {
                      [strong.controlDelegate didFailControlOfKind:DSBlockKindImage
                                                           action:@"download"
                                                          message:error.localizedDescription];
                      return;
                    }
                    [strong.controlDelegate didPressControlOfKind:DSBlockKindImage
                                                      blockIndex:strong.segment.blockIndex
                                                      blockCount:1
                                                        language:@""
                                                         action:@"download"
                                                         format:@""
                                                       filename:[NSString stringWithUTF8String:name.c_str()]];
                  }];
  });
}

- (NSInteger)linkIndexAtPoint:(CGPoint)point {
  // An image that is a link's hit area is the only thing here that can be a
  // link, so the answer is the box's own index rather than a text hit test.
  for (DSImageBoxView *box in _boxes) {
    if (box.isLink && !box.picture.hidden && CGRectContainsPoint(box.frame, point)) {
      return box.linkIndex;
    }
  }
  return -1;
}

@end

#pragma mark - One formula

/// The view that draws one display list. Separate from the scroll view around it
/// so the drawing keeps no scroll state of its own and can be laid out once.
@interface DSMathCanvasView : UIView
@property (nonatomic, nullable) MathLayout::Handle layout;
@property (nonatomic) CGFloat fontSize;
/// The theme's padding, which is where the formula's box starts inside the canvas.
@property (nonatomic) CGFloat padding;
@property (nonatomic, strong, nullable) UIColor *color;
@property (nonatomic, strong, nullable) UIColor *backgroundColor;
@end

@implementation DSMathCanvasView

- (instancetype)initWithFrame:(CGRect)frame {
  if (!(self = [super initWithFrame:frame])) {
    return nil;
  }
  // Transparent, because `drawRect:` only fills what the theme asks for and an
  // opaque view shows black wherever it does not draw.
  self.opaque = NO;
  self.contentMode = UIViewContentModeRedraw;
  return self;
}

- (void)setLayout:(MathLayout::Handle)layout {
  if (_layout == layout) {
    return;
  }
  _layout = layout;
  [self setNeedsDisplay];
}

- (void)setColor:(UIColor *)color {
  _color = color;
  [self setNeedsDisplay];
}

- (void)setBackgroundColor:(UIColor *)backgroundColor {
  _backgroundColor = backgroundColor;
  [self setNeedsDisplay];
}

- (void)drawRect:(CGRect)rect {
  [super drawRect:rect];
  if (self.backgroundColor != nil) {
    CGContextRef context = UIGraphicsGetCurrentContext();
    CGContextSetFillColorWithColor(context, self.backgroundColor.CGColor);
    CGContextFillRect(context, rect);
  }
  if (self.layout == nil) {
    return;
  }
  // Vectors, at the size the theme and the font scale gave. The layout is in em
  // units and carries no theme, so the color below is the run's own dynamic
  // color and dark mode reaches a formula without a relayout.
  CGContextRef context = UIGraphicsGetCurrentContext();
  CGContextSaveGState(context);
  CGContextTranslateCTM(context, self.padding, self.padding);
  [DSMathRenderer drawLayout:*self.layout
                        size:self.fontSize
                       color:self.color ?: UIColor.labelColor
                       scale:1
                   inContext:context];
  CGContextRestoreGState(context);
}

@end

@interface DSMathSegmentView ()
@property (nonatomic, nullable) DSMathSegment *segment;
@property (nonatomic, strong, nullable) UIScrollView *scroll;
@property (nonatomic, strong, nullable) DSMathCanvasView *canvas;
@property (nonatomic, strong, nullable) UILabel *fallback;
@end

@implementation DSMathSegmentView

- (instancetype)initWithFrame:(CGRect)frame {
  if (!(self = [super initWithFrame:frame])) {
    return nil;
  }
  _scroll = [[UIScrollView alloc] initWithFrame:self.bounds];
  // Bounces off, and never vertically: a formula scrolls sideways, and a vertical
  // bounce inside a chat list fights the list's own scroll. The drag rules are the
  // code card's — this takes a horizontal drag while it can scroll that way and
  // refuses at the edge, so the list's pan takes over.
  _scroll.bounces = NO;
  _scroll.showsHorizontalScrollIndicator = NO;
  _scroll.showsVerticalScrollIndicator = NO;
  _scroll.alwaysBounceHorizontal = NO;
  [self addSubview:_scroll];
  _canvas = [DSMathCanvasView new];
  [_scroll addSubview:_canvas];
  // The fallback, for a formula with no layout: the source as a reader saw it
  // before math existed, in the error color so a failure is visible at a glance.
  _fallback = [[UILabel alloc] initWithFrame:self.bounds];
  _fallback.numberOfLines = 0;
  _fallback.font = [UIFont monospacedSystemFontOfSize:15 weight:UIFontWeightRegular];
  _fallback.textColor = UIColor.secondaryLabelColor;
  _fallback.hidden = YES;
  [self addSubview:_fallback];
  return self;
}

- (void)setSegment:(DSMathSegment *)segment {
  _segment = segment;
  const BOOL laid = segment.layout != nil;
  self.canvas.hidden = !laid;
  self.scroll.hidden = !laid;
  self.fallback.hidden = laid;
  NSString *literal = [NSString stringWithUTF8String:segment.literal.c_str()] ?: @"";
  if (!laid) {
    // The literal, which is `Block::text` verbatim: delimiters included, exactly
    // what the writer sent, and what a reader saw before math existed.
    self.fallback.text = literal;
    // The error color only for a formula that failed. One that is still arriving
    // has no layout yet and nothing wrong with it, so it reads in the text color.
    self.fallback.textColor = !segment.error.empty() ? (segment.errorColor ?: UIColor.secondaryLabelColor)
                                                      : (segment.color ?: UIColor.labelColor);
    self.accessibilityLabel = [NSString stringWithFormat:@"math, %@", literal];
    self.isAccessibilityElement = YES;
    self.accessibilityTraits = UIAccessibilityTraitStaticText;
    [self setNeedsLayout];
    return;
  }
  self.canvas.layout = segment.layout;
  self.canvas.fontSize = segment.fontSize;
  self.canvas.padding = segment.padding;
  [self.canvas setNeedsDisplay];
  self.canvas.color = segment.color;
  self.canvas.backgroundColor = segment.backgroundColor;
  // The spoken form comes from the engine's own verbalizer, because KaTeX's
  // answer is hidden MathML and no native API reads it. The literal is the
  // fallback when the spoken form says nothing, which is what the reader's
  // character navigation lands on.
  NSString *spoken = DSMathSpeechForTex(segment.tex, true);
  self.accessibilityLabel = spoken.length > 0 ? spoken : literal;
  self.isAccessibilityElement = YES;
  self.accessibilityTraits = UIAccessibilityTraitStaticText;
  [self setNeedsLayout];
}

- (void)layoutSubviews {
  [super layoutSubviews];
  DSMathSegment *segment = self.segment;
  if (segment == nil) {
    return;
  }
  self.fallback.frame = self.bounds;
  if (segment.layout == nil) {
    return;
  }
  const CGFloat available = std::max<CGFloat>(0, CGRectGetWidth(self.bounds) - segment.contentLeading);
  const CGFloat content = [segment contentWidth] + 2 * segment.padding;
  const CGFloat height = [segment height];
  // A formula narrower than the column is centred by default, which is what a
  // display formula is. A wider one starts at the leading edge and scrolls,
  // which is what KaTeX does and what keeps a wide matrix readable.
  const CGFloat x = content > available || segment.textAlign < 0.5
      ? segment.contentLeading
      : segment.textAlign > 1.5 ? CGRectGetWidth(self.bounds) - content
                                 : segment.contentLeading + (available - content) / 2;
  self.scroll.frame = CGRectMake(x, 0, std::min(available, content), height);
  self.canvas.frame = CGRectMake(0, 0, content, height);
  self.scroll.contentSize = CGSizeMake(content, height);
}

- (NSInteger)linkIndexAtPoint:(CGPoint)point {
  // A formula holds no links. KaTeX renders `\href` as text for exactly this
  // reason, and a formula that looks tappable and is not would be worse than one
  // that plainly is not.
  return -1;
}

@end
