#import "DSRenderedDocument.h"

#include "Controls.h"
#include "SegmentPlan.h"
#include "TokenPalette.h"

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
/// The row a code block's buttons sit in, and the row a table's sit in. Fixed
/// in both directions so a block does not move when a button appears.
constexpr CGFloat kChromeRowHeight = 32;
/// Padding inside a code card, the same on every side.
constexpr CGFloat kCodePadding = 10;
constexpr CGFloat kMinColumnWidth = 56;
constexpr CGFloat kMaxColumnWidth = 280;
constexpr CGFloat kCellPaddingHorizontal = 12;
constexpr CGFloat kCellPaddingVertical = 8;

/// One dynamic color per token class, so switching appearance needs no
/// rebuild. Plain and Operator share the code text color, as they do in the
/// engine's palette.
UIColor *tokenColor(downstream::TokenClass token) {
  static NSArray<UIColor *> *colors;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    NSMutableArray<UIColor *> *built = [NSMutableArray arrayWithCapacity:downstream::kTokenColorsCount];
    for (size_t index = 0; index < downstream::kTokenColorsCount; index++) {
      const downstream::TokenColors &entry = downstream::kTokenColors[index];
      const uint32_t light = entry.light;
      const uint32_t dark = entry.dark;
      [built addObject:[UIColor colorWithDynamicProvider:^UIColor *(UITraitCollection *traits) {
        const BOOL night = traits.userInterfaceStyle == UIUserInterfaceStyleDark;
        return [UIColor colorWithRed:((night ? dark : light) >> 16 & 0xff) / 255.0
                               green:((night ? dark : light) >> 8 & 0xff) / 255.0
                                blue:((night ? dark : light) & 0xff) / 255.0
                               alpha:((night ? dark : light) >> 24 & 0xff) / 255.0];
      }]];
    }
    colors = [built copy];
  });
  return colors[static_cast<size_t>(token)];
}

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

/// Find the first strong letter. Digits and punctuation do not decide the
/// direction, so a numbered Arabic or Hebrew item still starts on the right.
NSWritingDirection writingDirection(const std::u16string &text) {
  NSCharacterSet *letters = NSCharacterSet.letterCharacterSet;
  for (size_t i = 0; i < text.size(); i++) {
    UTF32Char scalar = text[i];
    if (scalar >= 0xd800 && scalar <= 0xdbff && i + 1 < text.size() &&
        text[i + 1] >= 0xdc00 && text[i + 1] <= 0xdfff) {
      scalar = 0x10000 + ((scalar - 0xd800) << 10) + (text[++i] - 0xdc00);
    }
    if (![letters longCharacterIsMember:scalar]) {
      continue;
    }
    const BOOL rtl = (scalar >= 0x0590 && scalar <= 0x08ff) ||
        (scalar >= 0xfb1d && scalar <= 0xfdff) ||
        (scalar >= 0xfe70 && scalar <= 0xfeff) ||
        (scalar >= 0x10800 && scalar <= 0x10fff) ||
        (scalar >= 0x1e800 && scalar <= 0x1efff);
    return rtl ? NSWritingDirectionRightToLeft : NSWritingDirectionLeftToRight;
  }
  return NSWritingDirectionNatural;
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

/// Applies a code block's token colors. Tokens are offsets into the block's own
/// text and only change color, never a character, so selection, copy, and the
/// accessibility text are untouched.
void applyTokens(NSMutableAttributedString *text, const Block &block, NSUInteger offset, uint32_t clampStart,
                 uint32_t clampEnd) {
  for (const TokenRun &run : block.tokens) {
    const uint32_t start = std::max(run.start, clampStart);
    const uint32_t end = std::min(run.end, clampEnd);
    if (start >= end) {
      continue;
    }
    [text addAttribute:NSForegroundColorAttributeName
                 value:tokenColor(run.token)
                 range:NSMakeRange(offset + start - clampStart, end - start)];
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
@property (nonatomic) BOOL rightToLeft;
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
  /// One lock and one cached layout per width, so asking for the height and the
  /// used width of the same width costs one `NSLayoutManager` pass rather than
  /// two. A code card asks for both.
  NSLock *_measurementLock;
  CGFloat _measuredWidth;
  CGFloat _measuredHeight;
  CGFloat _measuredUsedWidth;
  BOOL _hasMeasurement;
}

/// `inCard` is true for the one-block body of a code card: the card draws the
/// background and the padding, and code is laid out left to right however the
/// block's own characters read.
+ (instancetype)segmentWithDocument:(const Document &)document
                               from:(size_t)from
                                 to:(size_t)to
                              scale:(CGFloat)scale
                         markerFont:(UIFont *)markerFont
                  syntaxHighlighting:(BOOL)syntaxHighlighting
                             inCard:(BOOL)inCard {
  DSTextSegment *segment = [DSTextSegment new];
  segment->_measurementLock = [NSLock new];
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
    NSWritingDirection direction = inCard ? NSWritingDirectionLeftToRight : writingDirection(block.text);
    if (direction == NSWritingDirectionNatural && blocks.count > 0) {
      direction = blocks.lastObject.rightToLeft ? NSWritingDirectionRightToLeft
                                                 : NSWritingDirectionLeftToRight;
    }
    info.rightToLeft = direction == NSWritingDirectionRightToLeft;
    info.decoration = block.kind == BlockKind::ThematicBreak ? DSBlockDecorationRule : DSBlockDecorationNone;

    NSMutableArray<DSContainerInfo *> *containers = [NSMutableArray new];
    NSMutableArray<NSString *> *spoken = [NSMutableArray new];
    bool inQuote = false;
    const CGFloat leading = buildContainers(block, previous, scale, markerFont, containers, spoken, &inQuote);
    info.containers = containers;
    info.contentLeading = leading;
    info.accessibilityPrefix = spoken.count > 0 ? [spoken componentsJoinedByString:@", "] : nil;

    // The segment's first block starts at the segment frame; the document
    // stack owns the gap above it.
    // A code card is its own segment, so its body never carries the padding
    // that used to be reserved inside the prose flow, and the block that
    // follows one starts from the plain gap the document stack adds.
    CGFloat gap = 0;
    if (previous != nullptr && !inCard) {
      gap = baseGap(block, scale);
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
    style.baseWritingDirection = direction;
    style.firstLineHeadIndent = info.rightToLeft ? 0 : leading;
    style.headIndent = style.firstLineHeadIndent;
    style.tailIndent = -(info.rightToLeft ? leading : 0);
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
    if (isCode && syntaxHighlighting) {
      applyTokens(text, block, start, 0, static_cast<uint32_t>(block.text.size()));
    }
    [blocks addObject:info];
    [blockRanges addObject:[NSValue valueWithRange:NSMakeRange(start, blockText.length)]];
  }

  segment.attributedString = [text copy];
  segment.blocks = [blocks copy];
  segment.blockRanges = [blockRanges copy];
  return segment;
}

/// Lays the text out once at `width` and hands back the measurements callers
/// need, so height and used width come from the same layout.
static void layoutText(DSTextSegment *segment, NSAttributedString *text, CGFloat width, CGFloat *height,
                       CGFloat *usedWidth) {
  DSTextKitStack *stack = [[DSTextKitStack alloc] initWithLayoutManager:[NSLayoutManager new]];
  stack.textContainer.size = CGSizeMake(std::isfinite(width) ? width : CGFLOAT_MAX, CGFLOAT_MAX);
  [stack.textStorage setAttributedString:text];
  [stack.layoutManager ensureLayoutForTextContainer:stack.textContainer];
  const CGRect used = [stack.layoutManager usedRectForTextContainer:stack.textContainer];
  // Unrounded, so a run split into several segments stacks to the same height
  // as one segment; the document rounds the segment edges.
  if (height != nullptr) {
    *height = CGRectGetMaxY(used);
  }
  if (usedWidth != nullptr) {
    *usedWidth = CGRectGetWidth(used);
  }
}

/// Measures at `width` if this width has not been measured yet, and hands back
/// whichever of the two values the caller needs. One layout answers both, which
/// is what a code card asks for on every measure and every unconstrained width.
- (void)measureAtWidth:(CGFloat)width height:(CGFloat *)height usedWidth:(CGFloat *)usedWidth {
  if (_attributedString.length == 0) {
    if (height != nullptr) {
      *height = 0;
    }
    if (usedWidth != nullptr) {
      *usedWidth = 0;
    }
    return;
  }
  [_measurementLock lock];
  if (_hasMeasurement && _measuredWidth == width) {
    if (height != nullptr) {
      *height = _measuredHeight;
    }
    if (usedWidth != nullptr) {
      *usedWidth = _measuredUsedWidth;
    }
    [_measurementLock unlock];
    return;
  }
  [_measurementLock unlock];

  CGFloat measuredHeight = 0;
  CGFloat measuredUsedWidth = 0;
  layoutText(self, _attributedString, width, &measuredHeight, &measuredUsedWidth);

  // Two threads can measure the same width at once. The text and the width are
  // the same, so both passes produce the same numbers, and the later write is
  // as good as the first; each caller answers from its own pass either way.
  [_measurementLock lock];
  _measuredWidth = width;
  _measuredHeight = measuredHeight;
  _measuredUsedWidth = measuredUsedWidth;
  _hasMeasurement = YES;
  [_measurementLock unlock];

  if (height != nullptr) {
    *height = measuredHeight;
  }
  if (usedWidth != nullptr) {
    *usedWidth = measuredUsedWidth;
  }
}

- (CGFloat)heightForWidth:(CGFloat)width {
  CGFloat height = 0;
  [self measureAtWidth:width height:&height usedWidth:nullptr];
  return height;
}

- (CGFloat)usedWidthForWidth:(CGFloat)width {
  CGFloat usedWidth = 0;
  [self measureAtWidth:width height:nullptr usedWidth:&usedWidth];
  return usedWidth;
}

@end

#pragma mark - Code cards

@interface DSCodeSegment ()
@property (nonatomic) DSTextSegment *text;
@property (nonatomic) NSString *language;
@property (nonatomic) NSInteger blockIndex;
@property (nonatomic) uint32_t codeOrdinal;
@property (nonatomic) NSArray<DSContainerInfo *> *containers;
@property (nonatomic) CGFloat contentLeading;
@property (nonatomic) NSString *firstLine;
@end

@implementation DSCodeSegment

+ (instancetype)segmentWithDocument:(const Document &)document
                               index:(size_t)index
                              scale:(CGFloat)scale
                         markerFont:(UIFont *)markerFont
                  syntaxHighlighting:(BOOL)syntaxHighlighting {
  DSCodeSegment *segment = [DSCodeSegment new];
  const Block &block = document.blocks[index];
  segment.text = [DSTextSegment segmentWithDocument:document
                                              from:index
                                                to:index + 1
                                             scale:scale
                                        markerFont:markerFont
                                 syntaxHighlighting:syntaxHighlighting
                                            inCard:YES];
  segment.language = [NSString stringWithUTF8String:block.language.c_str()];
  segment.blockIndex = static_cast<NSInteger>(index);
  NSMutableArray<DSContainerInfo *> *containers = [NSMutableArray new];
  NSMutableArray<NSString *> *spoken = [NSMutableArray new];
  bool inQuote = false;
  segment.contentLeading = buildContainers(block, nullptr, scale, markerFont, containers, spoken, &inQuote);
  segment.containers = containers;
  const size_t newline = block.text.find(u'\n');
  segment.firstLine = toNSString(block.text.substr(0, newline == std::u16string::npos ? block.text.size() : newline));
  return segment;
}

- (CGFloat)headerHeight {
  return kChromeRowHeight;
}

/// The width the body is laid out at: the card's inner width when it wraps, and
/// unbounded when it does not, which is what makes a long line scroll instead
/// of wrapping. The card's own padding and the container's margin come off the
/// available width first, so a body never lays out wider than the card.
- (CGFloat)layoutWidthForWidth:(CGFloat)width wrapped:(BOOL)wrapped {
  if (!wrapped) {
    return CGFLOAT_MAX;
  }
  const CGFloat inset = self.contentLeading + 2 * kCodePadding;
  return std::max<CGFloat>(0, width - inset);
}

- (CGFloat)bodyHeightForWidth:(CGFloat)width wrapped:(BOOL)wrapped {
  return [self.text heightForWidth:[self layoutWidthForWidth:width wrapped:wrapped]];
}

- (CGFloat)bodyWidthForWidth:(CGFloat)width wrapped:(BOOL)wrapped {
  return [self.text usedWidthForWidth:[self layoutWidthForWidth:width wrapped:wrapped]];
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
@property (nonatomic) BOOL rightToLeft;
@property (nonatomic) uint32_t tableIdentifier;
@property (nonatomic) NSString *firstRowText;
@property (nonatomic) NSArray<DSContainerInfo *> *containers;
@property (nonatomic) NSArray<NSNumber *> *columnWidths;
@property (nonatomic) NSArray<NSNumber *> *rowHeights;
@property (nonatomic) CGSize tableSize;
@property (nonatomic) UIEdgeInsets cellPadding;
@property (nonatomic) NSInteger blockIndex;
@property (nonatomic) uint32_t blockCount;
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
  NSWritingDirection direction = NSWritingDirectionNatural;
  for (size_t index = from; index < to && direction == NSWritingDirectionNatural; index++) {
    direction = writingDirection(document.blocks[index].text);
  }
  segment.rightToLeft = direction == NSWritingDirectionRightToLeft;
  segment.tableIdentifier = first.table;
  segment.firstRowText = toNSString(first.text);
  segment.blockIndex = static_cast<NSInteger>(from);
  segment.blockCount = static_cast<uint32_t>(to - from);
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
  NSArray<NSNumber *> *_segmentBlockIndexes;
  uint32_t _settledBlocks;
  std::vector<CGFloat> _gaps;
  NSLock *_lock;
  CGFloat _cachedWidth;
  uint32_t _cachedChrome;
  /// The wrap choices the cached frames were measured under, so a measurement
  /// is only reused for the choices it was made with.
  BOOL _cachedCodeBlockWrap;
  std::vector<uint32_t> _cachedWrapToggles;
  bool _hasCachedFrames;
  NSArray<NSValue *> *_cachedFrames;
}

+ (NSCache<NSString *, DSRenderedDocument *> *)cache {
  static NSCache *cache;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    cache = [NSCache new];
    // Worker threads prepare every document in a commit before Fabric
    // measures them, so this holds at least a screenful of messages.
    cache.countLimit = 128;
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
               markerFont:(UIFont *)markerFont
        syntaxHighlighting:(BOOL)syntaxHighlighting {
  const BlockKind kind = document.blocks[from].kind;
  NSString *key = [NSString stringWithFormat:@"%llu:%zu:%.4f:%d", (unsigned long long)document.blocks[from].identity,
                                             to - from, scale, syntaxHighlighting];
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
  switch (kind) {
    case BlockKind::TableRow:
      cached->segment = (id)[DSTableSegment segmentWithDocument:document
                                                            from:from
                                                              to:to
                                                           scale:scale
                                                      markerFont:markerFont];
      break;
    case BlockKind::CodeBlock:
      cached->segment = (id)[DSCodeSegment segmentWithDocument:document
                                                         index:from
                                                        scale:scale
                                                   markerFont:markerFont
                                        syntaxHighlighting:syntaxHighlighting];
      break;
    default:
      cached->segment = (id)[DSTextSegment segmentWithDocument:document
                                                           from:from
                                                             to:to
                                                          scale:scale
                                                     markerFont:markerFont
                                          syntaxHighlighting:syntaxHighlighting
                                                         inCard:NO];
      break;
  }
  [self.committedSegmentCache setObject:cached forKey:key cost:cost];
  return cached->segment;
}

+ (instancetype)renderedDocumentFor:(const DocumentHandle &)document
                          fontScale:(CGFloat)fontScale
                 syntaxHighlighting:(BOOL)syntaxHighlighting {
  if (!document) {
    return [[self alloc] initWithDocument:document fontScale:fontScale syntaxHighlighting:syntaxHighlighting];
  }
  NSString *key = [NSString stringWithFormat:@"%llu:%.4f:%d", (unsigned long long)document->id, fontScale,
                                             syntaxHighlighting];
  DSRenderedDocument *rendered = [self.cache objectForKey:key];
  if (rendered == nil) {
    rendered = [[self alloc] initWithDocument:document
                                    fontScale:fontScale
                           syntaxHighlighting:syntaxHighlighting];
    [self.cache setObject:rendered forKey:key];
  }
  return rendered;
}

+ (void)invalidateCaches {
  [self.cache removeAllObjects];
  [self.committedSegmentCache removeAllObjects];
}

- (instancetype)initWithDocument:(const DocumentHandle &)documentHandle
                       fontScale:(CGFloat)rawScale
              syntaxHighlighting:(BOOL)syntaxHighlighting {
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
  NSMutableArray<NSNumber *> *indexes = [NSMutableArray new];
  _settledBlocks = document.settledBlocks;

  for (const SegmentPlan &segment : planSegments(document)) {
    id built = nil;
    if (segment.committed) {
      built = [DSRenderedDocument committedSegmentFor:document
                                                 from:segment.from
                                                   to:segment.to
                                                scale:scale
                                           markerFont:markerFont
                                    syntaxHighlighting:syntaxHighlighting];
    } else if (segment.isCode()) {
      built = [DSCodeSegment segmentWithDocument:document
                                           index:segment.from
                                          scale:scale
                                     markerFont:markerFont
                              syntaxHighlighting:syntaxHighlighting];
    } else if (segment.isTable()) {
      built = [DSTableSegment segmentWithDocument:document
                                             from:segment.from
                                               to:segment.to
                                            scale:scale
                                       markerFont:markerFont];
    } else {
      built = [DSTextSegment segmentWithDocument:document
                                            from:segment.from
                                              to:segment.to
                                           scale:scale
                                      markerFont:markerFont
                               syntaxHighlighting:syntaxHighlighting
                                          inCard:NO];
    }
    // The plan counted the code blocks, so the ordinal comes from there rather
    // than from a walk of the blocks. It is assigned here so a committed segment
    // taken from the cache gets it too, and cannot carry a stale one.
    if ([built isKindOfClass:DSCodeSegment.class] && segment.codeOrdinal >= 0) {
      ((DSCodeSegment *)built).codeOrdinal = static_cast<uint32_t>(segment.codeOrdinal);
    }
    [segments addObject:built];
    [indexes addObject:@((NSInteger)segment.from)];
    _gaps.push_back(segment.from == 0 ? 0 : baseGap(document.blocks[segment.from], scale));
  }
  _segments = [segments copy];
  _segmentBlockIndexes = [indexes copy];
  return self;
}

- (NSArray *)segments {
  return _segments;
}

- (NSArray<NSNumber *> *)segmentBlockIndexes {
  return _segmentBlockIndexes;
}

- (uint32_t)settledBlocks {
  return _settledBlocks;
}

/// The height of one segment at a view width, including whatever control row
/// the chrome puts above it.
///
/// A code block's own layout comes from the reader's wrap choices rather than
/// from `chrome`, because a document can hold wrapped and unwrapped code at
/// once and `chrome` says only whether every block wraps. The decision is
/// `downstream::codeWraps`, the same function the view and the worker use, so a
/// block's measured height and its drawn height cannot disagree.
- (CGFloat)heightOf:(id)segment
            atWidth:(CGFloat)width
             chrome:(uint32_t)chrome
       codeBlockWrap:(BOOL)codeBlockWrap
         wrapToggles:(const std::vector<uint32_t> &)wrapToggles {
  if ([segment isKindOfClass:DSTextSegment.class]) {
    return [(DSTextSegment *)segment heightForWidth:width];
  }
  if ([segment isKindOfClass:DSCodeSegment.class]) {
    DSCodeSegment *code = segment;
    const BOOL wrapped = codeWraps(codeBlockWrap, wrapToggles, code.codeOrdinal);
    const CGFloat body = [code bodyHeightForWidth:width wrapped:wrapped];
    return 2 * kCodePadding + body + ((chrome & kChromeCodeHeader) != 0 ? [code headerHeight] : 0);
  }
  DSTableSegment *table = segment;
  return table.tableSize.height + ((chrome & kChromeTableToolbar) != 0 ? kChromeRowHeight : 0);
}

/// The width an unconstrained measure takes: the widest thing that does not
/// fit a view, which is a table's columns or a code line that scrolls.
- (CGFloat)unconstrainedWidthOf:(id)segment
                        atWidth:(CGFloat)width
                  codeBlockWrap:(BOOL)codeBlockWrap
                    wrapToggles:(const std::vector<uint32_t> &)wrapToggles {
  if ([segment isKindOfClass:DSCodeSegment.class]) {
    DSCodeSegment *code = segment;
    const BOOL wrapped = codeWraps(codeBlockWrap, wrapToggles, code.codeOrdinal);
    return code.contentLeading + 2 * kCodePadding + [code bodyWidthForWidth:width wrapped:wrapped];
  }
  if ([segment isKindOfClass:DSTableSegment.class]) {
    DSTableSegment *table = segment;
    return table.contentLeading + table.tableSize.width;
  }
  return 0;
}

- (NSArray<NSValue *> *)segmentFramesForWidth:(CGFloat)width
                                     chrome:(uint32_t)chrome
                              codeBlockWrap:(BOOL)codeBlockWrap
                                wrapToggles:(const std::vector<uint32_t> &)wrapToggles {
  // A measurement only holds for the wrap choices it was made under, so those
  // are part of the key. The set is empty in every document the reader has not
  // toggled, so this is one size comparison in practice.
  [_lock lock];
  if (_hasCachedFrames && _cachedWidth == width && _cachedChrome == chrome &&
      _cachedCodeBlockWrap == codeBlockWrap && _cachedWrapToggles == wrapToggles) {
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
    const CGFloat height = [self heightOf:segment
                                               atWidth:width
                                                chrome:chrome
                                          codeBlockWrap:codeBlockWrap
                                            wrapToggles:wrapToggles];
    unconstrainedWidth = std::max(unconstrainedWidth, [self unconstrainedWidthOf:segment
                                                                        atWidth:width
                                                                  codeBlockWrap:codeBlockWrap
                                                                    wrapToggles:wrapToggles]);
    const CGFloat top = std::ceil(y);
    // Never shorter than the segment. Rounding both edges up loses up to a
    // point when y is fractional, and a text view lays out only the lines that
    // fit its frame, so a segment a fraction short would drop its last line.
    const CGFloat bottom = std::max(std::ceil(y + height), top + std::ceil(height));
    [frames addObject:[NSValue valueWithCGRect:CGRectMake(0, top, 0, bottom - top)]];
    y += height;
  }

  // Every segment spans the view; an unconstrained measure uses the widest
  // segment that does not fit.
  const CGFloat frameWidth = std::isfinite(width) ? width : unconstrainedWidth;
  for (NSUInteger index = 0; index < frames.count; index++) {
    CGRect frame = frames[index].CGRectValue;
    frame.size.width = frameWidth;
    frames[index] = [NSValue valueWithCGRect:frame];
  }

  [_lock lock];
  _cachedWidth = width;
  _cachedChrome = chrome;
  _cachedCodeBlockWrap = codeBlockWrap;
  _cachedWrapToggles = wrapToggles;
  _hasCachedFrames = YES;
  _cachedFrames = frames;
  [_lock unlock];
  return frames;
}

- (CGSize)sizeForWidth:(CGFloat)width
                chrome:(uint32_t)chrome
         codeBlockWrap:(BOOL)codeBlockWrap
           wrapToggles:(const std::vector<uint32_t> &)wrapToggles {
  NSArray<NSValue *> *frames = [self segmentFramesForWidth:width
                                                    chrome:chrome
                                             codeBlockWrap:codeBlockWrap
                                               wrapToggles:wrapToggles];
  if (frames.count == 0) {
    return CGSizeZero;
  }
  const CGRect last = frames.lastObject.CGRectValue;
  return CGSizeMake(std::ceil(CGRectGetWidth(last)), std::ceil(CGRectGetMaxY(last)));
}

- (uint32_t)chromeForControls:(uint32_t)controls codeBlockWrap:(BOOL)codeBlockWrap {
  return downstream::chromeForProps(controls, codeBlockWrap, {});
}

@end
