#import "DSRenderedDocument.h"

#import "DSFonts.h"
#import "DSThemeColors.h"

#include "CodeLayout.h"
#include "Controls.h"
#include "InlineRuns.h"
#include "SegmentPlan.h"
#include "TokenPalette.h"
#include "theme/ThemeLayout.h"

#include <algorithm>
#include <cmath>

using namespace downstream;
using namespace downstream::theme;
using namespace downstream::theme_layout;

NSAttributedStringKey const DSBlockInfoAttributeName = @"DSBlockInfo";
NSAttributedStringKey const DSLinkIndexAttributeName = @"DSLinkIndex";

namespace {

/// A block's font, with a bold or italic run layered on top of it.
///
/// Raising the weight rather than replacing the face is what keeps a `strong`
/// inside a themed heading a heavier heading, instead of a heading that drops
/// back to the body face. Two steps for a bold that lands on an already-bold
/// weight, so bold in a bold heading still reads as emphasis.
UIFont *runFont(const BlockFont &font, CGFloat size, bool monospaced) {
  return DSMakeFont(font.family, size, font.weight, font.italic, monospaced);
}

/// The size an inline code span draws at, which follows a themed body size
/// unless the theme set an absolute one. A fraction of the block's own size, so
/// inline code keeps its proportions when the theme changes the body size.
CGFloat inlineCodeSizeFor(const Theme &theme, CGFloat blockSize, CGFloat scale) {
  if (inlineCodeIsAbsolute(theme)) {
    return theme_layout::inlineCodeSize(theme) * scale;
  }
  return blockSize * inlineCodeScale(theme);
}

NSString *toNSString(const std::u16string &text) {
  return [NSString stringWithCharacters:reinterpret_cast<const unichar *>(text.data()) length:text.size()];
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
///
/// Every color here is a slot read through the theme, with the platform's own
/// color as the fallback. That is what makes an unset slot inherit increased
/// contrast and appearance changes, which a frozen `UIColor` would not.
void applyRuns(NSMutableAttributedString *text, const Block &block, const BlockFont &font, const Theme &theme,
               DSThemeColors *colors, NSUInteger offset, CGFloat size, bool monospaced, uint32_t clampStart,
               uint32_t clampEnd) {
  const bool isCode = block.kind == BlockKind::CodeBlock;
  UIColor *inlineCodeBackground = [colors colorForSlot:kSlotCodeBackgroundColor fallback:UIColor.tertiarySystemFillColor];
  UIColor *footnoteColor = [colors colorForSlot:kSlotFootnoteColor fallback:UIColor.linkColor];
  const CGFloat footnoteSize = size * footnoteSizeScale(theme);
  for (const InlineRun &run : inlineRunsInRange(block.runs, clampStart, clampEnd)) {
    const uint32_t start = std::max(run.start, clampStart);
    const uint32_t end = std::min(run.end, clampEnd);
    if (start >= end) {
      continue;
    }
    const NSRange range = NSMakeRange(offset + start - clampStart, end - start);
    const bool code = (run.styles & InlineStyle::Code) != 0 && !isCode;
    const bool footnote = (run.styles & InlineStyle::FootnoteRef) != 0;
    const bool bold = (run.styles & InlineStyle::Strong) != 0;
    const bool italic = (run.styles & InlineStyle::Emphasis) != 0;

    // The run's face starts as its block's and takes the inline kind's slots on
    // top, so a `strong` in a themed heading is a heavier heading rather than a
    // heading that drops back to the body face.
    BlockFont runTypeface = font;
    CGFloat runSize = size;
    if (bold) {
      runTypeface = strongFont(theme, runTypeface);
    }
    if (italic) {
      runTypeface = emFont(theme, runTypeface);
    }
    if (code) {
      runTypeface = inlineCodeFont(theme, runTypeface);
      runSize = inlineCodeSizeFor(theme, size, 1);
    } else if (footnote) {
      runSize = footnoteSize;
    }
    if (run.link >= 0) {
      runTypeface = linkFont(theme, runTypeface);
    }

    NSMutableDictionary *attributes = [NSMutableDictionary new];
    if (runSize != size || runTypeface.family != font.family || runTypeface.weight != font.weight ||
        runTypeface.italic != font.italic || (code && !monospaced)) {
      attributes[NSFontAttributeName] = runFont(runTypeface, runSize, code || monospaced);
    }
    if (code) {
      attributes[NSBackgroundColorAttributeName] = inlineCodeBackground;
      attributes[NSForegroundColorAttributeName] =
          [colors colorForSlot:kSlotCodeColor fallback:nil] ?: attributes[NSForegroundColorAttributeName];
    }
    if (run.styles & InlineStyle::Strikethrough) {
      attributes[NSStrikethroughStyleAttributeName] = @(NSUnderlineStyleSingle);
      // A theme can recolor a strike without touching the text around it.
      UIColor *strike = [colors colorForSlot:kSlotStrikethroughColor fallback:nil];
      if (strike != nil) {
        attributes[NSStrikethroughColorAttributeName] = strike;
      }
    }
    if (run.styles & InlineStyle::ImageAlt) {
      // Image alt text reads as secondary by design, so the theme's own alt
      // color wins rather than the block's text color.
      attributes[NSForegroundColorAttributeName] =
          [colors colorForSlot:kSlotImageAltColor fallback:UIColor.secondaryLabelColor];
    }
    if (bold) {
      UIColor *strongColor = [colors colorForSlot:kSlotStrongColor fallback:nil];
      if (strongColor != nil) {
        attributes[NSForegroundColorAttributeName] = strongColor;
      }
    }
    if (italic) {
      UIColor *emColor = [colors colorForSlot:kSlotEmColor fallback:nil];
      if (emColor != nil) {
        attributes[NSForegroundColorAttributeName] = emColor;
      }
    }
    if (footnote) {
      attributes[NSBaselineOffsetAttributeName] = @(size * footnoteRaise(theme));
      attributes[NSForegroundColorAttributeName] = footnoteColor;
    }
    if (run.link >= 0) {
      attributes[NSForegroundColorAttributeName] =
          [colors colorForSlot:kSlotLinkColor fallback:UIColor.linkColor];
      // A theme can turn the underline off, which is what a link that reads as a
      // link by color alone wants.
      if (linkUnderlined(theme)) {
        attributes[NSUnderlineStyleAttributeName] = @(NSUnderlineStyleSingle);
        attributes[NSUnderlineColorAttributeName] =
            [colors colorForSlot:kSlotLinkColor fallback:UIColor.linkColor];
      }
      attributes[DSLinkIndexAttributeName] = @(run.link);
    }
    [text addAttributes:attributes range:range];
  }
}

/// Applies a code block's token colors. Tokens are offsets into the block's own
/// text and only change color, never a character, so selection, copy, and the
/// accessibility text are untouched.
void applyTokens(NSMutableAttributedString *text, const Block &block, DSThemeColors *colors, NSUInteger offset,
                 uint32_t clampStart, uint32_t clampEnd) {
  for (const TokenRun &run : block.tokens) {
    const uint32_t start = std::max(run.start, clampStart);
    const uint32_t end = std::min(run.end, clampEnd);
    if (start >= end) {
      continue;
    }
    const UIColor *token = [colors tokenColor:run.token fallback:nil];
    if (token == nil) {
      // A token the theme left unset keeps the code block's own color, which is
      // what a plain run keeps today. Plain and Operator land here.
      continue;
    }
    [text addAttribute:NSForegroundColorAttributeName
                 value:token
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
@property (nonatomic) CGFloat barWidth;
@property (nonatomic) CGFloat markerGap;
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
@property (nonatomic, nullable) DSDecorationMetrics *rule;
@property (nonatomic, nullable) NSString *accessibilityPrefix;
@end

@implementation DSBlockInfo
@end

@interface DSDecorationMetrics ()
@property (nonatomic) UIColor *color;
@property (nonatomic) CGFloat height;
@end

@implementation DSDecorationMetrics
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

/// An admonition's bar, from the slot its type names.
///
/// Each kind has its own slot, so a theme can make every alert read as one
/// family, or give each one its own color, which is why this is a slot lookup
/// rather than a fixed palette in the renderer. A slot the theme left unset
/// already resolves to its default role color, so a theme that sets no alert
/// colors gets the platform's five, which is what the renderer used before.
UIColor *admonitionColor(DSThemeColors *colors, const std::u16string &type) {
  const uint32_t slot = type == u"note"        ? kSlotAdmonitionNote
                        : type == u"tip"       ? kSlotAdmonitionTip
                        : type == u"important" ? kSlotAdmonitionImportant
                        : type == u"warning"  ? kSlotAdmonitionWarning
                        : type == u"caution"  ? kSlotAdmonitionCaution
                                               : 0;
  if (slot == 0) {
    // Not one of the five kinds GitHub defines, so it gets the neutral bar an
    // ordinary quote gets.
    return nil;
  }
  return [colors colorForSlot:slot fallback:nil];
}

/// Builds container decorations for a block and returns its content leading.
///
/// `previous` is the block before this one in the same segment, or null, which
/// is what decides whether a bar joins across the gap instead of restarting.
static CGFloat buildContainers(const Block &block, const Block *previous, const Theme &theme, DSThemeColors *colors,
                               CGFloat scale, UIFont *markerFont, UIColor *markerColor,
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
    info.barWidth = quoteBarWidth(theme) * scale;
    info.markerGap = markerGap(theme) * scale;
    if (container.kind == ContainerKind::Quote || container.kind == ContainerKind::Admonition) {
      *inQuote = true;
      info.width = containerIndent(theme, container.kind) * scale;
      // An alert's own bar when its slot is set, and a quote's neutral bar
      // otherwise. A zero border width is honored by the draw sites, which is
      // how a theme removes bars without hiding the indent.
      info.barColor = container.kind == ContainerKind::Admonition
          ? (admonitionColor(colors, container.marker)
             ?: [colors colorForSlot:kSlotBlockquoteBorderColor fallback:UIColor.systemGray3Color])
          : [colors colorForSlot:kSlotBlockquoteBorderColor fallback:UIColor.systemGray3Color];
    } else {
      info.width = containerIndent(theme, container.kind) * scale;
      if (container.showsMarker) {
        NSString *marker = toNSString(container.marker);
        UIColor *color = markerColor;
        if (container.task == TaskState::Unchecked) {
          marker = @"\u2610";
          color = [colors colorForSlot:kSlotTaskListBorderColor fallback:markerColor];
          [spoken addObject:@"unchecked"];
        } else if (container.task == TaskState::Checked) {
          marker = @"\u2611";
          color = [colors colorForSlot:kSlotTaskListCheckedColor fallback:markerColor];
          [spoken addObject:@"checked"];
        } else if (container.kind == ContainerKind::Footnote || marker.length > 1) {
          [spoken addObject:marker];
        } else if (container.kind == ContainerKind::ListItem) {
          // An ordered item's number and a bullet share the list marker color
          // unless the theme separates them.
          // An ordered item's number takes the bullet's color slot, so a theme
          // can separate a bullet from a number, or leave them the one color a
          // list's marker slot already names.
          color = marker.length > 0 ? [colors colorForSlot:kSlotListBulletColor fallback:markerColor] : markerColor;
        }
        info.marker = [[NSAttributedString alloc] initWithString:marker
                                                      attributes:@{
                                                        NSFontAttributeName : markerFont,
                                                        NSForegroundColorAttributeName : color,
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
@property (nonatomic) DSThemeColors *theme;
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
                             theme:(const Theme &)theme
                              scale:(CGFloat)scale
                         markerFont:(UIFont *)markerFont
                  syntaxHighlighting:(BOOL)syntaxHighlighting
                             inCard:(BOOL)inCard {
  DSTextSegment *segment = [DSTextSegment new];
  segment->_measurementLock = [NSLock new];
  segment.theme = [DSThemeColors colorsFor:theme];
  DSThemeColors *colors = segment.theme;
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

    // The block's typography comes from the theme's slots for its element, so
    // a themed heading, paragraph and code block each draw with their own size,
    // weight, family and color rather than one table of constants.
    const BlockFont font = blockFont(theme, block);
    UIColor *markerColor = [colors colorForSlot:kSlotListMarkerColor fallback:UIColor.secondaryLabelColor];
    NSMutableArray<DSContainerInfo *> *containers = [NSMutableArray new];
    NSMutableArray<NSString *> *spoken = [NSMutableArray new];
    bool inQuote = false;
    const CGFloat leading =
        buildContainers(block, previous, theme, colors, scale, markerFont, markerColor, containers, spoken, &inQuote);
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
      gap = blockGap(theme, block.spacingBefore, previous, block) * scale;
    }
    info.spacingBefore = gap;

    // A block inside a quote is drawn in the quote's own color, unless the theme
    // set a color for the block's own element, which is how a themed quote can
    // restyle the text inside it. It is asked per appearance, so a color the
    // theme named for dark mode only leaves light mode's quote text muted.
    UIColor *baseColor = [colors colorForSlot:font.colorSlot fallback:UIColor.labelColor];
    if (inQuote) {
      UIColor *quoteColor = [colors colorForSlot:kSlotBlockquoteColor fallback:UIColor.secondaryLabelColor];
      baseColor = [colors colorForSlot:font.colorSlot namedOr:quoteColor];
    }
    const CGFloat baseSize = font.size * scale;
    const bool monospaced = elementFor(block) == BlockElement::Code;
    if (isAdmonitionTitle(document, index)) {
      baseColor = admonitionColor(colors, block.containers.back().marker) ?: baseColor;
    }
    UIFont *baseFont = runFont(font, baseSize, monospaced);
    if (block.kind == BlockKind::ThematicBreak) {
      // A rule's color and thickness are the theme's, and the layout manager
      // reads them off the block rather than from a literal of its own.
      DSDecorationMetrics *rule = [DSDecorationMetrics new];
      rule.color = [colors colorForSlot:kSlotThematicBreakColor fallback:UIColor.separatorColor];
      rule.height = theme.number(kSlotThematicBreakHeight) * scale;
      info.rule = rule;
    }

    NSMutableParagraphStyle *style = [NSMutableParagraphStyle new];
    style.lineBreakMode = NSLineBreakByWordWrapping;
    style.baseWritingDirection = direction;
    style.firstLineHeadIndent = info.rightToLeft ? 0 : leading;
    style.headIndent = style.firstLineHeadIndent;
    style.tailIndent = -(info.rightToLeft ? leading : 0);
    // A themed line height is set as a minimum rather than an exact height, so
    // a line that the platform needs more room for still gets it and no glyph
    // is clipped. A line height below the face's own is ignored by the platform
    // anyway, so the default of zero and a small value behave alike.
    style.minimumLineHeight = font.lineHeight * scale;
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

    applyRuns(text, block, font, theme, colors, start, baseSize, monospaced, 0,
              static_cast<uint32_t>(block.text.size()));
    if (isCode && syntaxHighlighting) {
      applyTokens(text, block, colors, start, 0, static_cast<uint32_t>(block.text.size()));
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
@property (nonatomic) NSString *title;
/// The parsed meta the engine read from the fence's info string, kept whole so
/// the view resolves numbers and the cap from the same value the measurement did.
@property (nonatomic) downstream::FenceMeta fenceMeta;
@property (nonatomic) const std::vector<uint32_t> *lineStarts;
@property (nonatomic) uint32_t lineCount;
@property (nonatomic) CGFloat digitAdvance;
@property (nonatomic) NSInteger blockIndex;
@property (nonatomic) uint32_t codeOrdinal;
@property (nonatomic) NSArray<DSContainerInfo *> *containers;
@property (nonatomic) CGFloat contentLeading;
@property (nonatomic) NSString *firstLine;
@property (nonatomic) DSThemeColors *theme;
@property (nonatomic) CGFloat padding;
@property (nonatomic) CGFloat borderRadius;
@property (nonatomic) CGFloat borderWidth;
@property (nonatomic) UIFont *headerFont;
@property (nonatomic, nullable) UIColor *headerButtonColor;
@property (nonatomic, nullable) UIColor *headerBorderColor;
@property (nonatomic) CGFloat lineHeight;
/// The code font at this scale, which the gutter draws its numbers in so a
/// number's digits line up with the code's own monospaced columns.
@property (nonatomic) UIFont *codeFont;
/// Owns the line starts the segment's pointer above refers to.
@property (nonatomic) std::vector<uint32_t> *ownedLineStarts;
@end

@implementation DSCodeSegment

+ (instancetype)segmentWithDocument:(const Document &)document
                               index:(size_t)index
                             theme:(const Theme &)theme
                              scale:(CGFloat)scale
                         markerFont:(UIFont *)markerFont
                  syntaxHighlighting:(BOOL)syntaxHighlighting {
  DSCodeSegment *segment = [DSCodeSegment new];
  const Block &block = document.blocks[index];
  segment.text = [DSTextSegment segmentWithDocument:document
                                              from:index
                                                to:index + 1
                                             theme:theme
                                              scale:scale
                                        markerFont:markerFont
                                 syntaxHighlighting:syntaxHighlighting
                                            inCard:YES];
  segment.language = [NSString stringWithUTF8String:block.language.c_str()];
  // The header row's own typography and its two remaining colors, all from the
  // theme rather than from literals in the card view.
  const BlockFont headerFont = codeHeaderFont(theme);
  segment.headerFont = DSMakeFont(headerFont.family, headerFont.size * scale, headerFont.weight, headerFont.italic,
                                  true);
  // A theme that names the button color gets it; one that names only the row's
  // text color gets buttons that match, because a header and its controls read
  // as one row. Both default to the same secondary label.
  DSThemeColors *colors = segment.theme;
  segment.headerButtonColor =
      theme.slotSet(kSlotCodeBlockHeaderButtonColor)
          ? [colors colorForSlot:kSlotCodeBlockHeaderButtonColor]
          : [colors colorForSlot:kSlotCodeBlockHeaderColor];
  segment.headerBorderColor = [colors colorForSlot:kSlotCodeBlockHeaderBorderColor];
  // The engine parsed the info string once; the header label, the gutter, and the
  // download name all read this struct rather than each scanning the raw string.
  segment.title = [NSString stringWithUTF8String:block.meta.title.c_str()] ?: @"";
  segment.fenceMeta = block.meta;
  // Found here, on whichever thread built the rendering, so the gutter's draw
  // pass never searches the text for newlines. Owned because the committed
  // segment cache outlives the block list it was built from.
  segment.ownedLineStarts = new std::vector<uint32_t>(codeLineStarts(block.text));
  segment.lineStarts = segment.ownedLineStarts;
  segment.lineCount = static_cast<uint32_t>(segment.ownedLineStarts->size());
  {
    // The code font at this scale, which is both the metric the gutter measures
    // its numbers with and the font it draws them in. Themed, so a theme that
    // resizes code resizes its line numbers with it.
    const BlockFont codeTypeface = codeFont(theme);
    UIFont *const face = runFont(codeTypeface, codeTypeface.size * scale, true);
    segment.codeFont = face;
    // One digit's advance, which is what makes the gutter exactly as wide as its
    // widest number needs and no wider.
    segment.digitAdvance = [@"0" sizeWithAttributes:@{NSFontAttributeName : face}].width;
    // The body's own line height rather than the font's, because TextKit adds
    // the leading the body actually drew with. Read from the measurement this
    // segment caches anyway, so it costs no extra layout pass.
    const CGFloat lines = std::max<uint32_t>(segment.lineCount, 1);
    segment.lineHeight = [segment.text heightForWidth:CGFLOAT_MAX] / lines;
  }
  segment.blockIndex = static_cast<NSInteger>(index);
  segment.theme = segment.text.theme;
  segment.padding = codeCardPadding(theme) * scale;
  segment.borderRadius = codeCardRadius(theme) * scale;
  segment.borderWidth = codeCardBorderWidth(theme) * scale;
  NSMutableArray<DSContainerInfo *> *containers = [NSMutableArray new];
  NSMutableArray<NSString *> *spoken = [NSMutableArray new];
  bool inQuote = false;
  segment.contentLeading = buildContainers(block, nullptr, theme, segment.theme, scale, markerFont,
                                           [segment.theme colorForSlot:kSlotListMarkerColor
                                                           fallback:UIColor.secondaryLabelColor],
                                           containers, spoken, &inQuote);
  segment.containers = containers;
  const size_t newline = block.text.find(u'\n');
  segment.firstLine = toNSString(block.text.substr(0, newline == std::u16string::npos ? block.text.size() : newline));
  return segment;
}

- (CGFloat)headerHeight {
  return theme_layout::kChromeRowHeight;
}

- (void)dealloc {
  delete _ownedLineStarts;
}

/// The width the body is laid out at: the card's inner width when it wraps, and
/// unbounded when it does not, which is what makes a long line scroll instead
/// of wrapping. The card's own padding, the gutter, and the container's margin
/// come off the available width first, so a body never lays out wider than the
/// card.
///
/// The gutter narrows the width a wrapped body wraps at, which is the only way
/// line numbers change a block's height. An unwrapped body's height is its line
/// count times its line height regardless, so a rotation still lays no unwrapped
/// code out again.
- (CGFloat)layoutWidthForCard:(CGFloat)cardWidth wrapped:(BOOL)wrapped gutterWidth:(CGFloat)gutterWidth {
  if (!wrapped) {
    return CGFLOAT_MAX;
  }
  // The card's own padding is part of the inset, so a theme that pads a card
  // wider makes the body narrower by exactly that much rather than overflowing.
  const CGFloat inset = self.contentLeading + 2 * self.padding + gutterWidth;
  return std::max<CGFloat>(0, cardWidth - inset);
}

- (CGFloat)gutterWidthForLayout:(const downstream::CodeBlockLayout &)layout {
  if (!layout.lineNumbers) {
    return 0;
  }
  const uint32_t digits = gutterDigits(layout.firstNumber, self.lineCount);
  return digits * self.digitAdvance + downstream::kGutterGap;
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
@property (nonatomic) DSThemeColors *theme;
@property (nonatomic) CGFloat borderWidth;
@property (nonatomic) CGFloat borderRadius;
@property (nonatomic, nullable) UIColor *rowBackgroundColor;
@property (nonatomic, nullable) UIColor *toolbarButtonColor;
@end

@implementation DSTableSegment

+ (instancetype)segmentWithDocument:(const Document &)document
                               from:(size_t)from
                                 to:(size_t)to
                             theme:(const Theme &)theme
                              scale:(CGFloat)scale
                         markerFont:(UIFont *)markerFont {
  DSTableSegment *segment = [DSTableSegment new];
  segment.theme = [DSThemeColors colorsFor:theme];
  segment.borderWidth = tableBorderWidth(theme) * scale;
  segment.borderRadius = tableBorderRadius(theme) * scale;
  const Block &first = document.blocks[from];
  NSMutableArray<DSContainerInfo *> *containers = [NSMutableArray new];
  NSMutableArray<NSString *> *spoken = [NSMutableArray new];
  bool inQuote = false;
  segment.contentLeading = buildContainers(first, nullptr, theme, segment.theme, scale, markerFont,
                                           [segment.theme colorForSlot:kSlotListMarkerColor
                                                           fallback:UIColor.secondaryLabelColor],
                                           containers, spoken, &inQuote);
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
  segment.cellPadding = UIEdgeInsetsMake(tableCellPaddingVertical(theme) * scale,
                                         tableCellPaddingHorizontal(theme) * scale,
                                         tableCellPaddingVertical(theme) * scale,
                                         tableCellPaddingHorizontal(theme) * scale);
  // A theme that sets no zebra striping has no row fills at all, which is a
  // different thing from setting them transparent, and the grid view checks
  // for nil rather than comparing to clear.
  segment.rowBackgroundColor = [segment.theme colorForSlot:kSlotTableRowEvenBackgroundColor fallback:nil];
  segment.toolbarButtonColor = [segment.theme colorForSlot:kSlotTableToolbarButtonColor];

  // A table's own typography, from the table slots, and its header row from the
  // header slots, so a themed table can restyle its body and its head apart.
  const BlockFont font = blockFont(theme, first);
  const CGFloat size = font.size * scale;
  const BlockFont headerFont = tableHeaderFont(theme);
  UIFont *bodyFace = runFont(font, size, false);
  UIFont *headerFace = runFont(headerFont, headerFont.size * scale, false);
  UIColor *color = [segment.theme colorForSlot:font.colorSlot fallback:UIColor.labelColor];
  UIColor *headerColor = [segment.theme colorForSlot:kSlotTableHeaderTextColor fallback:color];
  NSMutableArray *rows = [NSMutableArray arrayWithCapacity:to - from];
  size_t columns = 0;

  for (size_t index = from; index < to; index++) {
    const Block &block = document.blocks[index];
    const bool headerRow = segment.hasHeader && index == from;
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
                NSFontAttributeName : headerRow ? headerFace : bodyFace,
                NSForegroundColorAttributeName : headerRow ? headerColor : color,
                NSParagraphStyleAttributeName : style,
              }];
      applyRuns(text, block, headerRow ? headerFont : font, theme, segment.theme, 0, size, false, cell.start,
                cell.end);
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
  const CGFloat maxWidth = tableMaxColumnWidth(theme) * scale;
  std::vector<CGFloat> widths(columns, tableMinColumnWidth(theme) * scale);
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
  // One border width for the closing grid line, so the last row and column are
  // not clipped by the view that draws the outer edge.
  segment.tableSize = CGSizeMake(std::ceil(tableWidth) + segment.borderWidth,
                                 std::ceil(tableHeight) + segment.borderWidth);
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

@interface DSRenderedDocument ()
@property (nonatomic) DSThemeColors *theme;
@end

@implementation DSRenderedDocument {
  NSArray *_segments;
  NSArray<NSNumber *> *_segmentBlockIndexes;
  uint32_t _settledBlocks;
  std::vector<CGFloat> _gaps;
  NSLock *_lock;
  CGFloat _cachedWidth;
  uint32_t _cachedChrome;
  /// The code layout the cached frames were measured under, so a measurement is
  /// only reused for the layout it was made with.
  downstream::CodeLayout _cachedCode;
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
                    theme:(const Theme &)theme
                    scale:(CGFloat)scale
               markerFont:(UIFont *)markerFont
        syntaxHighlighting:(BOOL)syntaxHighlighting {
  const BlockKind kind = document.blocks[from].kind;
  // The theme is in the key because a segment holds resolved colors and
  // measured sizes, and a second theme must not read the first one's.
  NSString *key = [NSString stringWithFormat:@"%llu:%zu:%llu:%.4f:%d", (unsigned long long)document.blocks[from].identity,
                                             to - from, (unsigned long long)theme.id(), scale, syntaxHighlighting];
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
                                                           theme:theme
                                                           scale:scale
                                                      markerFont:markerFont];
      break;
    case BlockKind::CodeBlock:
      cached->segment = (id)[DSCodeSegment segmentWithDocument:document
                                                         index:from
                                                        theme:theme
                                                        scale:scale
                                                   markerFont:markerFont
                                        syntaxHighlighting:syntaxHighlighting];
      break;
    default:
      cached->segment = (id)[DSTextSegment segmentWithDocument:document
                                                           from:from
                                                             to:to
                                                          theme:theme
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
                 syntaxHighlighting:(BOOL)syntaxHighlighting
                              theme:(const Theme::Handle &)themeHandle {
  if (!document) {
    return [[self alloc] initWithDocument:document
                               fontScale:fontScale
                      syntaxHighlighting:syntaxHighlighting
                                   theme:themeHandle ? themeHandle : Theme::defaultTheme()];
  }
  // The theme id is in the key rather than the theme itself, because interned
  // themes are stable for the life of a process and their id is all a cache key
  // needs to tell two of them apart.
  NSString *key = [NSString stringWithFormat:@"%llu:%.4f:%d:%llu", (unsigned long long)document->id, fontScale,
                                             syntaxHighlighting,
                                             (unsigned long long)(themeHandle ? themeHandle->id()
                                                                            : Theme::defaultTheme()->id())];
  DSRenderedDocument *rendered = [self.cache objectForKey:key];
  if (rendered == nil) {
    rendered = [[self alloc] initWithDocument:document
                                   fontScale:fontScale
                          syntaxHighlighting:syntaxHighlighting
                                    theme:themeHandle ? themeHandle : Theme::defaultTheme()];
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
              syntaxHighlighting:(BOOL)syntaxHighlighting
                            theme:(const Theme::Handle &)themeHandle {
  if (!(self = [super init])) {
    return nil;
  }
  _lock = [NSLock new];
  _cachedWidth = -1;
  const Theme &theme = *themeHandle;
  // A theme can cap the platform's font scale but not raise it, so the cap is
  // applied here once and every measurement below works with the result.
  const CGFloat scale = static_cast<CGFloat>(fontScale(theme, rawScale > 0 ? rawScale : 1));
  const Document emptyDocument;
  const Document &document = documentHandle ? *documentHandle : emptyDocument;
  self.theme = [DSThemeColors colorsFor:theme];
  // A marker uses the theme's own marker slots. With none set it draws as the
  // body did before theming: monospaced digits at the body's size, which is what
  // makes a numbered list's numbers line up column to column.
  const BlockFont marker = listMarkerFont(theme);
  const BOOL monospaced = marker.family.empty() && theme.font(kSlotCodeBlockFontFamily).empty();
  UIFont *markerFont = DSMakeFont(marker.family,
                                  (marker.size > 0 ? marker.size : paragraphFont(theme).size) * scale,
                                  marker.weight, false, monospaced);
  NSMutableArray *segments = [NSMutableArray new];
  NSMutableArray<NSNumber *> *indexes = [NSMutableArray new];
  _settledBlocks = document.settledBlocks;

  for (const SegmentPlan &segment : planSegments(document)) {
    id built = nil;
    if (segment.committed) {
      built = [DSRenderedDocument committedSegmentFor:document
                                                 from:segment.from
                                                   to:segment.to
                                                theme:theme
                                                scale:scale
                                           markerFont:markerFont
                                    syntaxHighlighting:syntaxHighlighting];
    } else if (segment.isCode()) {
      built = [DSCodeSegment segmentWithDocument:document
                                           index:segment.from
                                          theme:theme
                                          scale:scale
                                     markerFont:markerFont
                              syntaxHighlighting:syntaxHighlighting];
    } else if (segment.isTable()) {
      built = [DSTableSegment segmentWithDocument:document
                                             from:segment.from
                                               to:segment.to
                                            theme:theme
                                            scale:scale
                                       markerFont:markerFont];
    } else {
      built = [DSTextSegment segmentWithDocument:document
                                            from:segment.from
                                              to:segment.to
                                           theme:theme
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
    _gaps.push_back(segment.from == 0 ? 0 : blockGap(theme, document.blocks[segment.from].spacingBefore,
                                                     &document.blocks[segment.from - 1],
                                                     document.blocks[segment.from]) * scale);
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
                code:(const downstream::CodeLayout &)code {
  if ([segment isKindOfClass:DSTextSegment.class]) {
    return [(DSTextSegment *)segment heightForWidth:width];
  }
  if ([segment isKindOfClass:DSCodeSegment.class]) {
    DSCodeSegment *card = segment;
    // The one definition: the props, the reader's choices, and this block's own
    // fence meta all meet in `resolveCodeBlock`, and `codeCardHeight` is the only
    // function that turns that into a height. The view calls the same two, which
    // is what keeps a measured card and a drawn card the same size.
    const downstream::CodeBlockLayout layout = resolveCodeBlock(code, card.codeOrdinal, card.fenceMeta);
    const CGFloat gutter = [card gutterWidthForLayout:layout];
    const CGFloat body =
        [card.text heightForWidth:[card layoutWidthForCard:width wrapped:layout.wraps gutterWidth:gutter]];
    return codeCardHeight(layout, (chrome & kChromeCodeHeader) != 0 ? [card headerHeight] : 0, card.padding, body,
                          card.lineHeight);
  }
  DSTableSegment *table = segment;
  return table.tableSize.height + ((chrome & kChromeTableToolbar) != 0 ? theme_layout::kChromeRowHeight : 0);
}

/// The width an unconstrained measure takes: the widest thing that does not
/// fit a view, which is a table's columns or a code line that scrolls.
- (CGFloat)unconstrainedWidthOf:(id)segment
                        atWidth:(CGFloat)width
                           code:(const downstream::CodeLayout &)code {
  if ([segment isKindOfClass:DSCodeSegment.class]) {
    DSCodeSegment *card = segment;
    const downstream::CodeBlockLayout layout = resolveCodeBlock(code, card.codeOrdinal, card.fenceMeta);
    const CGFloat gutter = [card gutterWidthForLayout:layout];
    return card.contentLeading + 2 * card.padding + gutter +
        [card.text usedWidthForWidth:[card layoutWidthForCard:width
                                                      wrapped:layout.wraps
                                                 gutterWidth:gutter]];
  }
  if ([segment isKindOfClass:DSTableSegment.class]) {
    DSTableSegment *table = segment;
    return table.contentLeading + table.tableSize.width;
  }
  return 0;
}

- (NSArray<NSValue *> *)segmentFramesForWidth:(CGFloat)width
                                     chrome:(uint32_t)chrome
                                        code:(const downstream::CodeLayout &)code {
  // A measurement only holds for the layout it was made under, so the whole code
  // layout is part of the key. The choice sets are empty in every document the
  // reader has not toggled, so this is one comparison in practice.
  [_lock lock];
  if (_hasCachedFrames && _cachedWidth == width && _cachedChrome == chrome && _cachedCode == code) {
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
  const BOOL constrained = std::isfinite(width);
  for (NSUInteger index = 0; index < _segments.count; index++) {
    y += _gaps[index];
    id segment = _segments[index];
    const CGFloat height = [self heightOf:segment atWidth:width chrome:chrome code:code];
    if (!constrained) {
      unconstrainedWidth = std::max(unconstrainedWidth, [self unconstrainedWidthOf:segment
                                                                             atWidth:width
                                                                                code:code]);
    }
    const CGFloat top = std::ceil(y);
    // Never shorter than the segment. Rounding both edges up loses up to a
    // point when y is fractional, and a text view lays out only the lines that
    // fit its frame, so a segment a fraction short would drop its last line.
    const CGFloat bottom = std::max(std::ceil(y + height), top + std::ceil(height));
    [frames addObject:[NSValue valueWithCGRect:CGRectMake(0, top, constrained ? width : 0, bottom - top)]];
    y += height;
  }

  // Every segment spans the view; an unconstrained measure uses the widest
  // segment that does not fit.
  if (!constrained) {
    for (NSUInteger index = 0; index < frames.count; index++) {
      CGRect frame = frames[index].CGRectValue;
      frame.size.width = unconstrainedWidth;
      frames[index] = [NSValue valueWithCGRect:frame];
    }
  }

  [_lock lock];
  _cachedWidth = width;
  _cachedChrome = chrome;
  _cachedCode = code;
  _hasCachedFrames = YES;
  _cachedFrames = frames;
  [_lock unlock];
  return frames;
}

- (CGSize)sizeForWidth:(CGFloat)width chrome:(uint32_t)chrome code:(const downstream::CodeLayout &)code {
  NSArray<NSValue *> *frames = [self segmentFramesForWidth:width chrome:chrome code:code];
  if (frames.count == 0) {
    return CGSizeZero;
  }
  const CGRect last = frames.lastObject.CGRectValue;
  return CGSizeMake(std::ceil(CGRectGetWidth(last)), std::ceil(CGRectGetMaxY(last)));
}

@end
