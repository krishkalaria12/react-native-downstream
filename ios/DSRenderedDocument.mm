#import "DSRenderedDocument.h"

#include <algorithm>
#include <cmath>

using namespace downstream;

NSAttributedStringKey const DSBlockInfoAttributeName = @"DSBlockInfo";
NSAttributedStringKey const DSLinkIndexAttributeName = @"DSLinkIndex";

namespace {

// Base metrics in points at font scale 1.
constexpr CGFloat kBodySize = 16;
constexpr CGFloat kCodeSize = 14;
constexpr CGFloat kHeadingSizes[] = {28, 24, 20, 18, 16, 15};
constexpr CGFloat kGapNormal = 12;
constexpr CGFloat kGapHeading = 20;
constexpr CGFloat kGapTight = 4;
constexpr CGFloat kQuoteIndent = 16;
constexpr CGFloat kListIndent = 32;
constexpr CGFloat kCodePadding = 10;
constexpr CGFloat kTableColumn = 112;

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

UIColor *barColorForAdmonition(const std::u16string &type) {
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

@implementation DSRenderedDocument {
  NSAttributedString *_attributedString;
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

+ (instancetype)renderedDocumentFor:(const DocumentHandle &)document fontScale:(CGFloat)fontScale {
  if (!document) {
    return [[self alloc] initWithDocument:Document{} fontScale:fontScale];
  }
  NSString *key = [NSString stringWithFormat:@"%llu:%.4f", (unsigned long long)document->id, fontScale];
  DSRenderedDocument *rendered = [self.cache objectForKey:key];
  if (rendered == nil) {
    rendered = [[self alloc] initWithDocument:*document fontScale:fontScale];
    [self.cache setObject:rendered forKey:key];
  }
  return rendered;
}

- (instancetype)initWithDocument:(const Document &)document fontScale:(CGFloat)rawScale {
  if (!(self = [super init])) {
    return nil;
  }

  const CGFloat scale = rawScale > 0 ? rawScale : 1;
  NSMutableAttributedString *text = [NSMutableAttributedString new];
  NSMutableArray<DSBlockInfo *> *blocks = [NSMutableArray arrayWithCapacity:document.blocks.size()];
  NSMutableArray<NSValue *> *blockRanges = [NSMutableArray arrayWithCapacity:document.blocks.size()];
  UIFont *markerFont = [UIFont monospacedDigitSystemFontOfSize:kBodySize * scale weight:UIFontWeightRegular];
  const Block *previous = nullptr;

  for (size_t index = 0; index < document.blocks.size(); index++) {
    const Block &block = document.blocks[index];
    const bool isCode = block.kind == BlockKind::CodeBlock;
    const bool isHeading = block.kind == BlockKind::Heading;
    const bool inQuote = std::any_of(block.containers.begin(), block.containers.end(), [](const Container &c) {
      return c.kind == ContainerKind::Quote || c.kind == ContainerKind::Admonition;
    });

    DSBlockInfo *info = [DSBlockInfo new];
    info.index = static_cast<NSInteger>(index);
    info.headingLevel = block.headingLevel;
    info.codePadding = isCode ? kCodePadding * scale : 0;
    switch (block.kind) {
      case BlockKind::CodeBlock:
        info.decoration = DSBlockDecorationCode;
        break;
      case BlockKind::ThematicBreak:
        info.decoration = DSBlockDecorationRule;
        break;
      case BlockKind::TableRow:
        info.decoration = block.tableHeader ? DSBlockDecorationTableHeader : DSBlockDecorationNone;
        break;
      default:
        info.decoration = DSBlockDecorationNone;
        break;
    }

    CGFloat leading = 0;
    NSMutableArray<DSContainerInfo *> *containers = [NSMutableArray arrayWithCapacity:block.containers.size()];
    NSMutableArray<NSString *> *spokenMarkers = [NSMutableArray new];
    for (size_t depth = 0; depth < block.containers.size(); depth++) {
      const Container &container = block.containers[depth];
      DSContainerInfo *containerInfo = [DSContainerInfo new];
      containerInfo.kind = static_cast<DSContainerKind>(container.kind);
      containerInfo.leading = leading;
      containerInfo.continuesPrevious = previous != nullptr && previous->containers.size() > depth &&
          previous->containers[depth].id == container.id;
      if (container.kind == ContainerKind::Quote || container.kind == ContainerKind::Admonition) {
        containerInfo.width = kQuoteIndent * scale;
        containerInfo.barColor = container.kind == ContainerKind::Admonition
            ? barColorForAdmonition(container.marker)
            : UIColor.systemGray3Color;
      } else {
        containerInfo.width = kListIndent * scale;
        if (container.showsMarker) {
          NSString *marker = toNSString(container.marker);
          if (container.task == TaskState::Unchecked) {
            marker = @"\u2610";
            [spokenMarkers addObject:@"unchecked"];
          } else if (container.task == TaskState::Checked) {
            marker = @"\u2611";
            [spokenMarkers addObject:@"checked"];
          } else if (container.kind == ContainerKind::Footnote || marker.length > 1) {
            [spokenMarkers addObject:marker];
          }
          containerInfo.marker = [[NSAttributedString alloc]
              initWithString:marker
                  attributes:@{
                    NSFontAttributeName : markerFont,
                    NSForegroundColorAttributeName : UIColor.secondaryLabelColor,
                  }];
        }
      }
      leading += containerInfo.width;
      [containers addObject:containerInfo];
    }
    info.containers = containers;
    info.contentLeading = leading;
    info.accessibilityPrefix = spokenMarkers.count > 0 ? [spokenMarkers componentsJoinedByString:@", "] : nil;

    CGFloat gap = 0;
    switch (block.spacingBefore) {
      case BlockSpacing::None:
        gap = 0;
        break;
      case BlockSpacing::Tight:
        gap = kGapTight * scale;
        break;
      case BlockSpacing::Normal:
        gap = (isHeading ? kGapHeading : kGapNormal) * scale;
        break;
    }
    if (previous != nullptr && previous->kind == BlockKind::CodeBlock) {
      gap += kCodePadding * scale;
    }
    if (isCode && previous != nullptr) {
      gap += kCodePadding * scale;
    }
    info.spacingBefore = gap;

    UIFont *baseFont;
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
    baseFont = makeFont(baseSize, false, false, monospaced, baseWeight);

    NSMutableParagraphStyle *style = [NSMutableParagraphStyle new];
    style.lineBreakMode = NSLineBreakByWordWrapping;
    style.baseWritingDirection = NSWritingDirectionNatural;
    style.firstLineHeadIndent = leading + info.codePadding;
    style.headIndent = leading + info.codePadding;
    style.tailIndent = -info.codePadding;
    if (block.kind == BlockKind::TableRow) {
      style.tabStops = @[];
      style.defaultTabInterval = kTableColumn * scale;
    }
    NSMutableParagraphStyle *firstStyle = [style mutableCopy];
    firstStyle.paragraphSpacingBefore = gap;

    NSString *blockText = block.text.empty() ? @"\u200b" : toNSString(block.text);
    const bool hasSeparator = index + 1 < document.blocks.size();
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
    NSRange firstParagraph = [text.string paragraphRangeForRange:NSMakeRange(start, 0)];
    firstParagraph = NSIntersectionRange(firstParagraph, blockRange);
    [text addAttribute:NSParagraphStyleAttributeName value:firstStyle range:firstParagraph];

    for (const InlineRun &run : block.runs) {
      const NSRange range = NSMakeRange(start + run.start, run.end - run.start);
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

    [blocks addObject:info];
    [blockRanges addObject:[NSValue valueWithRange:NSMakeRange(start, blockText.length)]];
    previous = &block;
  }

  _attributedString = [text copy];
  _blocks = [blocks copy];
  _blockRanges = [blockRanges copy];
  if (!document.blocks.empty()) {
    _topInset = document.blocks.front().kind == BlockKind::CodeBlock ? kCodePadding * scale : 0;
    _bottomInset = document.blocks.back().kind == BlockKind::CodeBlock ? kCodePadding * scale : 0;
  }
  return self;
}

- (NSAttributedString *)attributedString {
  return _attributedString;
}

- (CGSize)sizeForWidth:(CGFloat)width {
  if (_attributedString.length == 0) {
    return CGSizeZero;
  }
  DSTextKitStack *stack = [[DSTextKitStack alloc] initWithLayoutManager:[NSLayoutManager new]];
  stack.textContainer.size = CGSizeMake(std::isfinite(width) ? width : CGFLOAT_MAX, CGFLOAT_MAX);
  [stack.textStorage setAttributedString:_attributedString];
  [stack.layoutManager ensureLayoutForTextContainer:stack.textContainer];
  const CGRect used = [stack.layoutManager usedRectForTextContainer:stack.textContainer];
  return CGSizeMake(std::ceil(CGRectGetMaxX(used)), std::ceil(_topInset + CGRectGetMaxY(used) + _bottomInset));
}

@end
