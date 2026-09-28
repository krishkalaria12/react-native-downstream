#import "DSRenderedDocument.h"

#include "SegmentPlan.h"

#include <algorithm>
#include <cmath>

using namespace downstream;

NSAttributedStringKey const DSBlockInfoAttributeName = @"DSBlockInfo";
NSAttributedStringKey const DSLinkIndexAttributeName = @"DSLinkIndex";

namespace {

// Base metrics in points at font scale 1. Mirrored in RenderedDocument.kt.
constexpr CGFloat kBodySize = 16;
constexpr CGFloat kCodeSize = 14;
constexpr CGFloat kHeadingSizes[] = {28, 24, 20, 18, 16, 15};
constexpr CGFloat kGapNormal = 12;
constexpr CGFloat kGapHeading = 20;
constexpr CGFloat kGapTight = 4;
constexpr CGFloat kQuoteIndent = 16;
constexpr CGFloat kListIndent = 32;
constexpr CGFloat kCodePadding = 10;
constexpr CGFloat kMinColumnWidth = 56;
constexpr CGFloat kMaxColumnWidth = 280;
constexpr CGFloat kCellPaddingHorizontal = 12;
constexpr CGFloat kCellPaddingVertical = 8;

NSString *toNSString(const std::u16string &text) {
  return [NSString stringWithCharacters:reinterpret_cast<const unichar *>(text.data()) length:text.size()];
}

UIFont *makeFont(CGFloat size, bool bold, bool italic, bool monospaced, UIFontWeight baseWeight) {
  UIFont *font = monospaced ? [UIFont monospacedSystemFontOfSize:size weight:bold ? UIFontWeightBold : baseWeight]
                            : [UIFont systemFontOfSize:size weight:bold ? UIFontWeightBold : baseWeight];
  if (italic) {
    UIFontDescriptor *descriptor = [font.fontDescriptor
        fontDescriptorWithSymbolicTraits:font.fontDescriptor.symbolicTraits | UIFontDescriptorTraitItalic];
    if (descriptor != nil) {
      font = [UIFont fontWithDescriptor:descriptor size:size];
    }
  }
  return font;
}

/// GitHub alert colors; other quotes use a neutral bar.
UIColor *admonitionColor(const std::u16string &type) {
  if (type == u"note") {
    return UIColor.systemBlueColor;
  }
  if (type == u"tip") {
    return UIColor.systemGreenColor;
  }
  if (type == u"important") {
    return UIColor.systemPurpleColor;
  }
  if (type == u"warning") {
    return UIColor.systemOrangeColor;
  }
  if (type == u"caution") {
    return UIColor.systemRedColor;
  }
  return UIColor.systemGray3Color;
}

CGFloat baseGap(const Block &block, CGFloat scale) {
  switch (block.spacingBefore) {
    case BlockSpacing::None:
      return 0;
    case BlockSpacing::Tight:
      return kGapTight * scale;
    case BlockSpacing::Normal:
      return (block.kind == BlockKind::Heading ? kGapHeading : kGapNormal) * scale;
  }
  return 0;
}

/// Applies a block's inline runs within [clampStart, clampEnd) to text that
/// starts at `offset`.
void applyRuns(NSMutableAttributedString *text, const Block &block, NSUInteger offset, CGFloat baseSize,
               UIFontWeight baseWeight, bool monospaced, uint32_t clampStart, uint32_t clampEnd) {
  const bool isCode = block.kind == BlockKind::CodeBlock;
  for (const InlineRun &run : block.runs) {
    const uint32_t start = std::max(run.start, clampStart);
    const uint32_t end = std::min(run.end, clampEnd);
    if (start >= end) {
      continue;
    }
    const NSRange range = NSMakeRange(offset + start - clampStart, end - start);
    const bool code = (run.styles & InlineStyle::Code) != 0 && !isCode;
    const bool footnote = (run.styles & InlineStyle::FootnoteRef) != 0;
    CGFloat size = baseSize;
    if (code) {
      size = baseSize * (kCodeSize / kBodySize);
    } else if (footnote) {
      size = baseSize * 0.75;
    }
    NSMutableDictionary *attributes = [NSMutableDictionary new];
    attributes[NSFontAttributeName] = makeFont(size, (run.styles & InlineStyle::Strong) != 0,
                                               (run.styles & InlineStyle::Emphasis) != 0, code || monospaced,
                                               baseWeight);
    if (code) {
      attributes[NSBackgroundColorAttributeName] = UIColor.tertiarySystemFillColor;
    }
    if (run.styles & InlineStyle::Strikethrough) {
      attributes[NSStrikethroughStyleAttributeName] = @(NSUnderlineStyleSingle);
    }
    if (run.styles & InlineStyle::ImageAlt) {
      attributes[NSForegroundColorAttributeName] = UIColor.secondaryLabelColor;
    }
    if (footnote) {
      attributes[NSBaselineOffsetAttributeName] = @(baseSize * 0.35);
      attributes[NSForegroundColorAttributeName] = UIColor.linkColor;
    }
    if (run.link >= 0) {
      attributes[NSForegroundColorAttributeName] = UIColor.linkColor;
      attributes[NSUnderlineStyleAttributeName] = @(NSUnderlineStyleSingle);
      attributes[DSLinkIndexAttributeName] = @(run.link);
    }
    [text addAttributes:attributes range:range];
  }
}

} // namespace

@interface DSContainerInfo ()
@property (nonatomic) DSContainerKind kind;
@property (nonatomic) CGFloat leading;
@property (nonatomic) CGFloat width;
@property (nonatomic, nullable) NSAttributedString *marker;
@property (nonatomic) BOOL continuesPrevious;
@property (nonatomic, nullable) UIColor *barColor;
@end

@implementation DSContainerInfo
@end

@interface DSBlockInfo ()
@property (nonatomic) NSInteger index;
@property (nonatomic) DSBlockDecoration decoration;
@property (nonatomic) NSInteger headingLevel;
@property (nonatomic) CGFloat spacingBefore;
@property (nonatomic) CGFloat contentLeading;
@property (nonatomic) CGFloat codePadding;
@property (nonatomic) NSArray<DSContainerInfo *> *containers;
@property (nonatomic, nullable) NSString *accessibilityPrefix;
@end

@implementation DSBlockInfo
@end

@implementation DSTextKitStack

- (instancetype)initWithLayoutManager:(NSLayoutManager *)layoutManager {
  if (self = [super init]) {
    _textStorage = [NSTextStorage new];
    _layoutManager = layoutManager;
    _layoutManager.usesFontLeading = YES;
    _textContainer = [[NSTextContainer alloc] initWithSize:CGSizeMake(0, CGFLOAT_MAX)];
    _textContainer.lineFragmentPadding = 0;
    _textContainer.maximumNumberOfLines = 0;
    _textContainer.lineBreakMode = NSLineBreakByWordWrapping;
    [_layoutManager addTextContainer:_textContainer];
    [_textStorage addLayoutManager:_layoutManager];
  }
  return self;
}

@end

#pragma mark - Containers

/// Builds container decorations for a block and returns its content leading.
static CGFloat buildContainers(const Block &block, const Block *previous, CGFloat scale, UIFont *markerFont,
                               NSMutableArray<DSContainerInfo *> *containers, NSMutableArray<NSString *> *spoken,
                               bool *inQuote) {
  CGFloat leading = 0;
  for (size_t depth = 0; depth < block.containers.size(); depth++) {
    const Container &container = block.containers[depth];
    DSContainerInfo *info = [DSContainerInfo new];
    info.kind = static_cast<DSContainerKind>(container.kind);
    info.leading = leading;
    info.continuesPrevious =
        previous != nullptr && previous->containers.size() > depth && previous->containers[depth].id == container.id;
    if (container.kind == ContainerKind::Quote || container.kind == ContainerKind::Admonition) {
      *inQuote = true;
      info.width = kQuoteIndent * scale;
      info.barColor =
          container.kind == ContainerKind::Admonition ? admonitionColor(container.marker) : UIColor.systemGray3Color;
    } else {
      info.width = kListIndent * scale;
      if (container.showsMarker) {
        NSString *marker = toNSString(container.marker);
        if (container.task == TaskState::Unchecked) {
          marker = @"\u2610";
          [spoken addObject:@"unchecked"];
        } else if (container.task == TaskState::Checked) {
          marker = @"\u2611";
          [spoken addObject:@"checked"];
        } else if (container.kind == ContainerKind::Footnote || marker.length > 1) {
          [spoken addObject:marker];
        }
        info.marker = [[NSAttributedString alloc] initWithString:marker
                                                      attributes:@{
                                                        NSFontAttributeName : markerFont,
                                                        NSForegroundColorAttributeName : UIColor.secondaryLabelColor,
                                                      }];
      }
    }
    leading += info.width;
    [containers addObject:info];
  }
  return leading;
}

#pragma mark - Text segments

@interface DSTextSegment ()
@property (nonatomic) NSAttributedString *attributedString;
@property (nonatomic) CGFloat topInset;
@property (nonatomic) CGFloat bottomInset;
@property (nonatomic) NSArray<DSBlockInfo *> *blocks;
@property (nonatomic) NSArray<NSValue *> *blockRanges;
@end

@implementation DSTextSegment {
  NSLock *_heightLock;
  CGFloat _heightWidth;
  CGFloat _height;
  BOOL _hasHeight;
}

+ (instancetype)segmentWithDocument:(const Document &)document
                               from:(size_t)from
                                 to:(size_t)to
                              scale:(CGFloat)scale
                         markerFont:(UIFont *)markerFont {
  DSTextSegment *segment = [DSTextSegment new];
  segment->_heightLock = [NSLock new];
  NSMutableAttributedString *text = [NSMutableAttributedString new];
  NSMutableArray<DSBlockInfo *> *blocks = [NSMutableArray arrayWithCapacity:to - from];
  NSMutableArray<NSValue *> *blockRanges = [NSMutableArray arrayWithCapacity:to - from];

  for (size_t index = from; index < to; index++) {
    const Block &block = document.blocks[index];
    const Block *previous = index > from ? &document.blocks[index - 1] : nullptr;
    const bool isCode = block.kind == BlockKind::CodeBlock;

    DSBlockInfo *info = [DSBlockInfo new];
    info.index = static_cast<NSInteger>(index);
    info.headingLevel = block.headingLevel;
    info.codePadding = isCode ? kCodePadding * scale : 0;
    info.decoration = isCode ? DSBlockDecorationCode
        : block.kind == BlockKind::ThematicBreak ? DSBlockDecorationRule
                                                 : DSBlockDecorationNone;

    NSMutableArray<DSContainerInfo *> *containers = [NSMutableArray new];
    NSMutableArray<NSString *> *spoken = [NSMutableArray new];
    bool inQuote = false;
    const CGFloat leading = buildContainers(block, previous, scale, markerFont, containers, spoken, &inQuote);
    info.containers = containers;
    info.contentLeading = leading;
    info.accessibilityPrefix = spoken.count > 0 ? [spoken componentsJoinedByString:@", "] : nil;

    // The segment's first block starts at the segment frame; the document
    // stack owns the gap above it.
    CGFloat gap = 0;
    if (previous != nullptr) {
      gap = baseGap(block, scale);
      if (previous->kind == BlockKind::CodeBlock) {
        gap += kCodePadding * scale;
      }
      if (isCode) {
        gap += kCodePadding * scale;
      }
    }
    info.spacingBefore = gap;

    UIColor *baseColor = inQuote ? UIColor.secondaryLabelColor : UIColor.labelColor;
    CGFloat baseSize = kBodySize * scale;
    UIFontWeight baseWeight = UIFontWeightRegular;
    bool monospaced = false;
    switch (block.kind) {
      case BlockKind::Heading: {
        const size_t level = std::clamp<size_t>(block.headingLevel, 1, 6);
        baseSize = kHeadingSizes[level - 1] * scale;
        baseWeight = level <= 2 ? UIFontWeightBold : UIFontWeightSemibold;
        if (level == 6) {
          baseColor = UIColor.secondaryLabelColor;
        }
        break;
      }
      case BlockKind::CodeBlock:
      case BlockKind::HtmlBlock:
        baseSize = kCodeSize * scale;
        monospaced = true;
        break;
      default:
        break;
    }
    if (isAdmonitionTitle(document, index)) {
      baseColor = admonitionColor(block.containers.back().marker);
    }
    UIFont *baseFont = makeFont(baseSize, false, false, monospaced, baseWeight);

    NSMutableParagraphStyle *style = [NSMutableParagraphStyle new];
    style.lineBreakMode = NSLineBreakByWordWrapping;
    style.baseWritingDirection = NSWritingDirectionNatural;
    style.firstLineHeadIndent = leading + info.codePadding;
    style.headIndent = leading + info.codePadding;
    style.tailIndent = -info.codePadding;
    NSMutableParagraphStyle *firstStyle = [style mutableCopy];
    firstStyle.paragraphSpacingBefore = gap;

    NSString *blockText = block.text.empty() ? @"\u200b" : toNSString(block.text);
    const bool hasSeparator = index + 1 < to;
    const NSUInteger start = text.length;
    [text appendAttributedString:[[NSAttributedString alloc]
                                     initWithString:hasSeparator ? [blockText stringByAppendingString:@"\n"] : blockText
                                         attributes:@{
                                           NSFontAttributeName : baseFont,
                                           NSForegroundColorAttributeName : baseColor,
                                           NSParagraphStyleAttributeName : style,
                                           DSBlockInfoAttributeName : info,
                                         }]];
    const NSRange blockRange = NSMakeRange(start, text.length - start);
    const NSRange firstParagraph = [text.string paragraphRangeForRange:NSMakeRange(start, 0)];
    [text addAttribute:NSParagraphStyleAttributeName
                 value:firstStyle
                 range:NSIntersectionRange(firstParagraph, blockRange)];

    applyRuns(text, block, start, baseSize, baseWeight, monospaced, 0, static_cast<uint32_t>(block.text.size()));
    [blocks addObject:info];
    [blockRanges addObject:[NSValue valueWithRange:NSMakeRange(start, blockText.length)]];
  }

  segment.attributedString = [text copy];
  segment.blocks = [blocks copy];
  segment.blockRanges = [blockRanges copy];
  segment.topInset = document.blocks[from].kind == BlockKind::CodeBlock ? kCodePadding * scale : 0;
  segment.bottomInset = document.blocks[to - 1].kind == BlockKind::CodeBlock ? kCodePadding * scale : 0;
  return segment;
}

- (CGFloat)heightForWidth:(CGFloat)width {
  if (_attributedString.length == 0) {
    return 0;
  }
  [_heightLock lock];
  if (_hasHeight && _heightWidth == width) {
    const CGFloat height = _height;
    [_heightLock unlock];
    return height;
  }
  [_heightLock unlock];

  DSTextKitStack *stack = [[DSTextKitStack alloc] initWithLayoutManager:[NSLayoutManager new]];
  stack.textContainer.size = CGSizeMake(std::isfinite(width) ? width : CGFLOAT_MAX, CGFLOAT_MAX);
  [stack.textStorage setAttributedString:_attributedString];
  [stack.layoutManager ensureLayoutForTextContainer:stack.textContainer];
  const CGRect used = [stack.layoutManager usedRectForTextContainer:stack.textContainer];
  // Unrounded, so a run split into several segments stacks to the same
  // height as one segment; the document rounds the segment edges.
  const CGFloat height = _topInset + CGRectGetMaxY(used) + _bottomInset;
  [_heightLock lock];
  _heightWidth = width;
  _height = height;
  _hasHeight = YES;
  [_heightLock unlock];
  return height;
}

@end

#pragma mark - Table segments

@interface DSTableCell ()
@property (nonatomic) NSAttributedString *text;
@end

@implementation DSTableCell
@end

@interface DSTableSegment ()
@property (nonatomic) NSArray<NSArray<DSTableCell *> *> *rows;
@property (nonatomic) BOOL hasHeader;
@property (nonatomic) CGFloat contentLeading;
@property (nonatomic) NSArray<DSContainerInfo *> *containers;
@property (nonatomic) NSArray<NSNumber *> *columnWidths;
@property (nonatomic) NSArray<NSNumber *> *rowHeights;
@property (nonatomic) CGSize tableSize;
@property (nonatomic) UIEdgeInsets cellPadding;
@end

@implementation DSTableSegment

+ (instancetype)segmentWithDocument:(const Document &)document
                               from:(size_t)from
                                 to:(size_t)to
                              scale:(CGFloat)scale
                         markerFont:(UIFont *)markerFont {
  DSTableSegment *segment = [DSTableSegment new];
  const Block &first = document.blocks[from];
  NSMutableArray<DSContainerInfo *> *containers = [NSMutableArray new];
  NSMutableArray<NSString *> *spoken = [NSMutableArray new];
  bool inQuote = false;
  segment.contentLeading = buildContainers(first, nullptr, scale, markerFont, containers, spoken, &inQuote);
  segment.containers = containers;
  segment.hasHeader = first.tableHeader;
  segment.cellPadding = UIEdgeInsetsMake(kCellPaddingVertical * scale, kCellPaddingHorizontal * scale,
                                         kCellPaddingVertical * scale, kCellPaddingHorizontal * scale);

  const CGFloat size = kBodySize * scale;
  UIFont *font = makeFont(size, false, false, false, UIFontWeightRegular);
  UIColor *color = inQuote ? UIColor.secondaryLabelColor : UIColor.labelColor;
  NSMutableArray *rows = [NSMutableArray arrayWithCapacity:to - from];
  size_t columns = 0;

  for (size_t index = from; index < to; index++) {
    const Block &block = document.blocks[index];
    NSMutableArray<DSTableCell *> *row = [NSMutableArray arrayWithCapacity:block.cells.size()];
    for (const TableCell &cell : block.cells) {
      NSMutableParagraphStyle *style = [NSMutableParagraphStyle new];
      style.lineBreakMode = NSLineBreakByWordWrapping;
      style.baseWritingDirection = NSWritingDirectionNatural;
      switch (cell.align) {
        case CellAlign::Center:
          style.alignment = NSTextAlignmentCenter;
          break;
        case CellAlign::Right:
          style.alignment = NSTextAlignmentRight;
          break;
        default:
          style.alignment = NSTextAlignmentNatural;
          break;
      }
      NSMutableAttributedString *text = [[NSMutableAttributedString alloc]
          initWithString:toNSString(block.text.substr(cell.start, cell.end - cell.start))
              attributes:@{
                NSFontAttributeName : font,
                NSForegroundColorAttributeName : color,
                NSParagraphStyleAttributeName : style,
              }];
      applyRuns(text, block, 0, size, UIFontWeightRegular, false, cell.start, cell.end);
      DSTableCell *tableCell = [DSTableCell new];
      tableCell.text = text;
      [row addObject:tableCell];
    }
    columns = std::max(columns, block.cells.size());
    [rows addObject:row];
  }
  segment.rows = rows;

  // Each column is as wide as its widest cell, clamped, so every view width
  // produces the same table and wide tables scroll.
  const UIEdgeInsets padding = segment.cellPadding;
  const NSStringDrawingOptions options = NSStringDrawingUsesLineFragmentOrigin | NSStringDrawingUsesFontLeading;
  const CGFloat maxWidth = kMaxColumnWidth * scale;
  std::vector<CGFloat> widths(columns, kMinColumnWidth * scale);
  for (NSArray<DSTableCell *> *row in rows) {
    for (NSUInteger column = 0; column < row.count; column++) {
      const CGRect bounds = [row[column].text boundingRectWithSize:CGSizeMake(maxWidth, CGFLOAT_MAX)
                                                           options:options
                                                           context:nil];
      widths[column] = std::max(widths[column], std::min(std::ceil(bounds.size.width), maxWidth));
    }
  }
  NSMutableArray<NSNumber *> *columnWidths = [NSMutableArray arrayWithCapacity:columns];
  CGFloat tableWidth = 0;
  for (const CGFloat width : widths) {
    const CGFloat padded = width + padding.left + padding.right;
    [columnWidths addObject:@(padded)];
    tableWidth += padded;
  }

  NSMutableArray<NSNumber *> *rowHeights = [NSMutableArray arrayWithCapacity:rows.count];
  CGFloat tableHeight = 0;
  for (NSArray<DSTableCell *> *row in rows) {
    CGFloat height = 0;
    for (NSUInteger column = 0; column < row.count; column++) {
      const CGRect bounds = [row[column].text boundingRectWithSize:CGSizeMake(widths[column], CGFLOAT_MAX)
                                                           options:options
                                                           context:nil];
      height = std::max(height, std::ceil(bounds.size.height));
    }
    height += padding.top + padding.bottom;
    [rowHeights addObject:@(height)];
    tableHeight += height;
  }
  segment.columnWidths = columnWidths;
  segment.rowHeights = rowHeights;
  // One extra point for the closing grid line.
  segment.tableSize = CGSizeMake(std::ceil(tableWidth) + 1, std::ceil(tableHeight) + 1);
  return segment;
}

@end

#pragma mark - Document

// Committed blocks are immutable under their identity, and identities are
// never reused, so a segment built from committed blocks can be reused by
// every later snapshot of the stream. The identities guard against a stale
// entry under the same key.
@interface DSCommittedSegment : NSObject {
@public
  std::vector<uint64_t> identities;
  id segment;
}
@end

@implementation DSCommittedSegment
@end

@implementation DSRenderedDocument {
  NSArray *_segments;
  std::vector<CGFloat> _gaps;
  NSLock *_lock;
  CGFloat _cachedWidth;
  NSArray<NSValue *> *_cachedFrames;
}

+ (NSCache<NSString *, DSRenderedDocument *> *)cache {
  static NSCache *cache;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    cache = [NSCache new];
    cache.countLimit = 32;
  });
  return cache;
}

+ (NSCache<NSString *, DSCommittedSegment *> *)committedSegmentCache {
  static NSCache *cache;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    cache = [NSCache new];
    cache.countLimit = 512;
    cache.totalCostLimit = 32 * 1024 * 1024;
  });
  return cache;
}

+ (id)committedSegmentFor:(const Document &)document
                     from:(size_t)from
                       to:(size_t)to
                    scale:(CGFloat)scale
               markerFont:(UIFont *)markerFont {
  const bool isTable = document.blocks[from].kind == BlockKind::TableRow;
  NSString *key = [NSString stringWithFormat:@"%llu:%zu:%.4f", (unsigned long long)document.blocks[from].identity,
                                             to - from, scale];
  DSCommittedSegment *cached = [self.committedSegmentCache objectForKey:key];
  bool matches = cached != nil && cached->identities.size() == to - from;
  for (size_t index = from; matches && index < to; index++) {
    matches = cached->identities[index - from] == document.blocks[index].identity;
  }
  if (matches) {
    return cached->segment;
  }
  cached = [DSCommittedSegment new];
  NSUInteger cost = 0;
  for (size_t index = from; index < to; index++) {
    cached->identities.push_back(document.blocks[index].identity);
    cost += document.blocks[index].text.size() * 16 + 256;
  }
  cached->segment = isTable
      ? (id)[DSTableSegment segmentWithDocument:document from:from to:to scale:scale markerFont:markerFont]
      : (id)[DSTextSegment segmentWithDocument:document from:from to:to scale:scale markerFont:markerFont];
  [self.committedSegmentCache setObject:cached forKey:key cost:cost];
  return cached->segment;
}

+ (instancetype)renderedDocumentFor:(const DocumentHandle &)document fontScale:(CGFloat)fontScale {
  if (!document) {
    return [[self alloc] initWithDocument:document fontScale:fontScale];
  }
  NSString *key = [NSString stringWithFormat:@"%llu:%.4f", (unsigned long long)document->id, fontScale];
  DSRenderedDocument *rendered = [self.cache objectForKey:key];
  if (rendered == nil) {
    rendered = [[self alloc] initWithDocument:document fontScale:fontScale];
    [self.cache setObject:rendered forKey:key];
  }
  return rendered;
}

- (instancetype)initWithDocument:(const DocumentHandle &)documentHandle fontScale:(CGFloat)rawScale {
  if (!(self = [super init])) {
    return nil;
  }
  _lock = [NSLock new];
  _cachedWidth = -1;
  const CGFloat scale = rawScale > 0 ? rawScale : 1;
  const Document emptyDocument;
  const Document &document = documentHandle ? *documentHandle : emptyDocument;
  UIFont *markerFont = [UIFont monospacedDigitSystemFontOfSize:kBodySize * scale weight:UIFontWeightRegular];
  NSMutableArray *segments = [NSMutableArray new];

  for (const SegmentPlan &segment : planSegments(document)) {
    if (segment.committed) {
      [segments addObject:[DSRenderedDocument committedSegmentFor:document
                                                             from:segment.from
                                                               to:segment.to
                                                            scale:scale
                                                       markerFont:markerFont]];
    } else if (segment.table) {
      [segments addObject:[DSTableSegment segmentWithDocument:document
                                                         from:segment.from
                                                           to:segment.to
                                                        scale:scale
                                                   markerFont:markerFont]];
    } else {
      [segments addObject:[DSTextSegment segmentWithDocument:document
                                                        from:segment.from
                                                          to:segment.to
                                                       scale:scale
                                                  markerFont:markerFont]];
    }
    _gaps.push_back(segment.from == 0 ? 0 : baseGap(document.blocks[segment.from], scale));
  }
  _segments = [segments copy];
  return self;
}

- (NSArray *)segments {
  return _segments;
}

- (NSArray<NSValue *> *)segmentFramesForWidth:(CGFloat)width {
  [_lock lock];
  if (_cachedFrames != nil && _cachedWidth == width) {
    NSArray *frames = _cachedFrames;
    [_lock unlock];
    return frames;
  }
  [_lock unlock];

  NSMutableArray<NSValue *> *frames = [NSMutableArray arrayWithCapacity:_segments.count];
  // Segment edges round up from their exact positions, so frames stay on
  // whole points without rounding error accumulating across segments.
  CGFloat y = 0;
  CGFloat unconstrainedWidth = 0;
  for (NSUInteger index = 0; index < _segments.count; index++) {
    y += _gaps[index];
    id segment = _segments[index];
    CGFloat height = 0;
    if ([segment isKindOfClass:DSTextSegment.class]) {
      height = [(DSTextSegment *)segment heightForWidth:width];
    } else {
      DSTableSegment *table = segment;
      height = table.tableSize.height;
      unconstrainedWidth = std::max(unconstrainedWidth, table.contentLeading + table.tableSize.width);
    }
    const CGFloat top = std::ceil(y);
    [frames addObject:[NSValue valueWithCGRect:CGRectMake(0, top, 0, std::ceil(y + height) - top)]];
    y += height;
  }

  // Every segment spans the view; an unconstrained measure uses the widest
  // table as the content width.
  const CGFloat frameWidth = std::isfinite(width) ? width : unconstrainedWidth;
  for (NSUInteger index = 0; index < frames.count; index++) {
    CGRect frame = frames[index].CGRectValue;
    frame.size.width = frameWidth;
    frames[index] = [NSValue valueWithCGRect:frame];
  }

  [_lock lock];
  _cachedWidth = width;
  _cachedFrames = frames;
  [_lock unlock];
  return frames;
}

- (CGSize)sizeForWidth:(CGFloat)width {
  NSArray<NSValue *> *frames = [self segmentFramesForWidth:width];
  if (frames.count == 0) {
    return CGSizeZero;
  }
  const CGRect last = frames.lastObject.CGRectValue;
  return CGSizeMake(std::ceil(CGRectGetWidth(last)), std::ceil(CGRectGetMaxY(last)));
}

@end
