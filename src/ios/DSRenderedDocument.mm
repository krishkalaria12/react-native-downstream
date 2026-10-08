#import "DSRenderedDocument.h"

#import "DSMathFonts.h"

#import "DSFonts.h"
#import "DSThemeColors.h"

#include "CodeLayout.h"
#include "Controls.h"
#include "InlineRuns.h"
#include "SegmentPlan.h"
#include "TokenPalette.h"
#include "Unicode.h"
#include "theme/ThemeLayout.h"

#include <algorithm>
#include <array>
#include <cmath>

using namespace downstream;
using namespace downstream::theme;
using namespace downstream::theme_layout;

NSAttributedStringKey const DSBlockInfoAttributeName = @"DSBlockInfo";
NSAttributedStringKey const DSLinkIndexAttributeName = @"DSLinkIndex";
NSAttributedStringKey const DSTagIndexAttributeName = @"DSTagIndex";
NSAttributedStringKey const DSCaretKindAttributeName = @"DSCaretKind";
NSString *const DSSpoilerAttributeName = @"DSSpoiler";

/// The character a caret reserves its space with.
///
/// U+FFFC OBJECT REPLACEMENT is what a text attachment occupies, and the
/// attachment carries explicit bounds rather than an image, so the placeholder's
/// height is the last character's own. Nothing is drawn from it: the shape comes
/// from the run's foreground color, which is how the caret takes the color of the
/// text it follows.
NSString *const kCaretPlaceholder = @"\uFFFC";

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

/// The writing direction a resolved block direction draws with.
///
/// The engine decided the direction; this only says how to hand it to TextKit.
/// `NSWritingDirectionNatural` is not an option, because it would let the
/// platform look at the text again and reach its own answer — which is the second
/// rule this replaced.
NSWritingDirection writingDirectionFor(downstream::BlockDirection direction) {
  switch (direction) {
    case downstream::BlockDirection::RightToLeft:
      return NSWritingDirectionRightToLeft;
    case downstream::BlockDirection::LeftToRight:
      return NSWritingDirectionLeftToRight;
    case downstream::BlockDirection::Inherit:
      break;
  }
  return NSWritingDirectionLeftToRight;
}

/// Whether any of this block's runs is raised or lowered: a footnote
/// reference, a `<sup>` or `<sub>`, or a `^x^` run.
bool holdsBaselineShift(const Block &block) {
  for (const InlineRun &run : block.runs) {
    if ((run.styles & (InlineStyle::FootnoteRef | InlineStyle::Superscript | InlineStyle::Subscript)) != 0) {
      return true;
    }
  }
  return false;
}

/// Whether the block carries an inline formula, whose attachment box a cap would
/// clip. A display formula is a block of its own and never reaches here.
bool hasInlineMath(const Block &block) { return !block.math.empty(); }

/// Applies a block's inline runs within [clampStart, clampEnd) to text that
/// starts at `offset`.
///
/// Every color here is a slot read through the theme, with the platform's own
/// color as the fallback. That is what makes an unset slot inherit increased
/// contrast and appearance changes, which a frozen `UIColor` would not.
void applyRuns(NSMutableAttributedString *text, const Block &block, const BlockFont &font, const Theme &theme,
               DSThemeColors *colors, NSUInteger offset, CGFloat size, bool monospaced, uint32_t clampStart,
               uint32_t clampEnd, const Document &document, DSLinkVariants *variants, DSTagStyles *tagStyles) {
  const auto runs = inlineRunsInRange(block.runs, clampStart, clampEnd);
  if (runs.empty()) {
    return;
  }
  const bool isCode = block.kind == BlockKind::CodeBlock;
  UIColor *inlineCodeBackground = [colors colorForSlot:kSlotCodeBackgroundColor fallback:UIColor.tertiarySystemFillColor];
  UIColor *footnoteColor = [colors colorForSlot:kSlotFootnoteColor fallback:UIColor.linkColor];
  const CGFloat footnoteSize = size * footnoteSizeScale(theme);
  // Offsets change between runs, but their attributes depend only on style and
  // link within this block. Bound the local cache so many distinct links cannot
  // retain an extra dictionary for every run.
  struct RunAttributes {
    uint32_t styles;
    int32_t link;
    int32_t tag;
    NSDictionary<NSAttributedStringKey, id> *attributes;
  };
  std::array<RunAttributes, 16> cachedAttributes{};
  size_t cachedCount = 0;
  for (const InlineRun &run : runs) {
    const uint32_t start = std::max(run.start, clampStart);
    const uint32_t end = std::min(run.end, clampEnd);
    if (start >= end) {
      continue;
    }
    const NSRange range = NSMakeRange(offset + start - clampStart, end - start);
    NSDictionary<NSAttributedStringKey, id> *cached = nil;
    for (size_t index = 0; index < cachedCount; ++index) {
      if (cachedAttributes[index].styles == run.styles && cachedAttributes[index].link == run.link &&
          cachedAttributes[index].tag == run.tag) {
        cached = cachedAttributes[index].attributes;
        break;
      }
    }
    if (cached != nil) {
      [text addAttributes:cached range:range];
      continue;
    }
    const bool code = (run.styles & InlineStyle::Code) != 0 && !isCode;
    const bool footnote = (run.styles & InlineStyle::FootnoteRef) != 0;
    const bool sup = (run.styles & InlineStyle::Superscript) != 0;
    const bool sub = (run.styles & InlineStyle::Subscript) != 0;
    const bool bold = (run.styles & InlineStyle::Strong) != 0;
    const bool italic = (run.styles & InlineStyle::Emphasis) != 0;
    // An app-styled tag's traits, read here so the face below takes them: the
    // name comes from the document's own tag table, which is the same string a
    // press reports. Color is applied with the other colors below, because it
    // writes the attributes directly rather than going through the face.
    long long tagFlags = 0;
    if (tagStyles != nil && !tagStyles.isEmpty && run.tag >= 0 &&
        static_cast<size_t>(run.tag) < document.tags.size()) {
      const std::string &tagName = document.tags[static_cast<size_t>(run.tag)].name;
      NSDictionary *tagStyle = [tagStyles styleForTag:@(tagName.c_str())];
      tagFlags = tagStyle != nil ? [tagStyle[@"flags"] longLongValue] : 0;
    }
    const bool tagBold = (tagFlags & 1) != 0;
    const bool tagItalic = (tagFlags & 2) != 0;

    // The run's face starts as its block's and takes the inline kind's slots on
    // top, so a `strong` in a themed heading is a heavier heading rather than a
    // heading that drops back to the body face.
    BlockFont runTypeface = font;
    CGFloat runSize = size;
    if (bold || tagBold) {
      runTypeface = strongFont(theme, runTypeface);
    }
    if (italic || tagItalic) {
      runTypeface = emFont(theme, runTypeface);
    }
    if (code) {
      runTypeface = inlineCodeFont(theme, runTypeface);
      runSize = inlineCodeSizeFor(theme, size, 1);
    } else if (footnote) {
      runSize = footnoteSize;
    } else if (sup) {
      runSize = size * superscriptFontScale(theme);
    } else if (sub) {
      runSize = size * subscriptFontScale(theme);
    }
    // Inline code keeps the code face and takes the script's scale too, so
    // `<sup>`code`</sup>` is raised as well as monospaced.
    if (code && sup) {
      runSize = inlineCodeSizeFor(theme, size, 1) * superscriptFontScale(theme);
    } else if (code && sub) {
      runSize = inlineCodeSizeFor(theme, size, 1) * subscriptFontScale(theme);
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
    // `<kbd>`: a key cap, which draws as inline code with a border. The border
    // comes from the background span the platform draws for it, so this only has
    // to set the face: the cap is what tells a reader it is a key.
    if (run.styles & InlineStyle::Keyboard) {
      runTypeface = inlineCodeFont(theme, runTypeface);
      runSize = inlineCodeSizeFor(theme, size, 1);
    }
    // `<mark>`: highlighted text, in the theme's own mark color over the block's
    // face. No background change: a highlight is ink, and the text under it stays
    // the text it was.
    if (run.styles & InlineStyle::Highlight) {
      UIColor *mark = [colors colorForSlot:kSlotMarkColor fallback:nil];
      if (mark != nil) {
        attributes[NSForegroundColorAttributeName] = mark;
      } else {
        attributes[NSBackgroundColorAttributeName] =
            [colors colorForSlot:kSlotMarkBackgroundColor fallback:UIColor.systemYellowColor];
      }
    }
    // `<u>` and `<ins>`: underlined text, in the theme's underline color where
    // it sets one.
    if (run.styles & InlineStyle::Underline) {
      attributes[NSUnderlineStyleAttributeName] = @(NSUnderlineStyleSingle);
      UIColor *underline = [colors colorForSlot:kSlotUnderlineColor fallback:nil];
      if (underline != nil) {
        attributes[NSUnderlineColorAttributeName] = underline;
      }
    }
    // `||spoiler||`: drawn as its own background until tapped, which is what
    // makes it hidden without removing it: the text is laid out, measured and
    // selectable, and only the ink is withheld. The marker attribute is what a
    // press reads to find the run, because the colors alone cannot say whether a
    // background is a spoiler's or a code span's.
    if (run.styles & InlineStyle::Spoiler) {
      attributes[NSBackgroundColorAttributeName] =
          [colors colorForSlot:kSlotSpoilerColor fallback:UIColor.labelColor];
      attributes[NSForegroundColorAttributeName] = [UIColor clearColor];
      attributes[DSSpoilerAttributeName] = @YES;
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
    if (sup) {
      attributes[NSBaselineOffsetAttributeName] = @(size * superscriptBaselineOffset(theme));
    }
    if (sub) {
      attributes[NSBaselineOffsetAttributeName] = @(-size * subscriptBaselineOffset(theme));
    }
    if (run.link >= 0) {
      // A per-URL variant wins over the theme: the first prefix the URL starts
      // with names the color, and anything it does not name stays the theme's.
      // The URL comes from the document's own link table, which is the same
      // string a press reports — so the style an app sees and the link an app
      // handles cannot disagree about which URL they mean.
      UIColor *linkColor = [colors colorForSlot:kSlotLinkColor fallback:UIColor.linkColor];
      BOOL linkUnderline = linkUnderlined(theme);
      if (variants != nil && !variants.isEmpty && run.link >= 0 &&
          static_cast<size_t>(run.link) < document.links.size()) {
        // The run's own link index is into `Document::links`, which is the same
        // string a press reports — so the style an app sees and the link an app
        // handles cannot disagree about which URL they mean.
        const std::string &url = document.links[static_cast<size_t>(run.link)].url;
        NSDictionary *variant = [variants variantForURL:@(url.c_str())];
        const long long variantColor = [variant[@"color"] longLongValue];
        if (variant != nil && variantColor >= 0) {
          linkColor = [UIColor colorWithRed:((variantColor >> 16) & 0xff) / 255.0
                                      green:((variantColor >> 8) & 0xff) / 255.0
                                       blue:(variantColor & 0xff) / 255.0
                                      alpha:((variantColor >> 24) & 0xff) / 255.0];
        }
        if (variant != nil && variant[@"underline"] != nil) {
          linkUnderline = [variant[@"underline"] boolValue];
        }
      }
      attributes[NSForegroundColorAttributeName] = linkColor;
      // A theme can turn the underline off, which is what a link that reads as a
      // link by color alone wants.
      if (linkUnderline) {
        attributes[NSUnderlineStyleAttributeName] = @(NSUnderlineStyleSingle);
        attributes[NSUnderlineColorAttributeName] = linkColor;
      }
      attributes[DSLinkIndexAttributeName] = @(run.link);
    }
    if (run.tag >= 0) {
      attributes[DSTagIndexAttributeName] = @(run.tag);
      // An app-styled tag draws with its entry's color and traits; anything it
      // does not name stays the surrounding text's. The name comes from the
      // document's own tag table, which is the same string a press reports.
      if (tagStyles != nil && !tagStyles.isEmpty &&
          static_cast<size_t>(run.tag) < document.tags.size()) {
        const std::string &tagName = document.tags[static_cast<size_t>(run.tag)].name;
        NSDictionary *style = [tagStyles styleForTag:@(tagName.c_str())];
        const long long tagColor = [style[@"color"] longLongValue];
        if (style != nil && tagColor >= 0) {
          attributes[NSForegroundColorAttributeName] = [UIColor
              colorWithRed:((tagColor >> 16) & 0xff) / 255.0
                     green:((tagColor >> 8) & 0xff) / 255.0
                      blue:(tagColor & 0xff) / 255.0
                     alpha:((tagColor >> 24) & 0xff) / 255.0];
        }
      }
    }
    NSDictionary<NSAttributedStringKey, id> *resolved = attributes;
    if (cachedCount < cachedAttributes.size()) {
      resolved = [attributes copy];
      cachedAttributes[cachedCount++] = {run.styles, run.link, run.tag, resolved};
    }
    [text addAttributes:resolved range:range];
  }
}

/// Applies a code block's token colors. Tokens are offsets into the block's own
/// text and only change color, never a character, so selection, copy, and the
/// accessibility text are untouched.
/// Swaps each laid-out inline formula's literal for one attachment character.
///
/// After the runs are applied, so the attachment inherits the run's font, color
/// and link, and from the last formula back, so an earlier formula's offsets are
/// still the block's own when it is reached. The literal rides along as an
/// attribute, which is what a copy writes back. A formula with no layout keeps
/// its literal, which is what a reader sees while it is still arriving and when
/// it failed.
void replaceInlineMath(NSMutableAttributedString *text, const Block &block, NSUInteger offset) {
  if (block.math.empty() || block.kind == BlockKind::MathBlock || block.kind == BlockKind::CodeBlock) {
    return;
  }
  for (auto span = block.math.rbegin(); span != block.math.rend(); ++span) {
    if (!span->layout || span->end <= span->start) {
      continue;
    }
    const NSRange range = NSMakeRange(offset + span->start, span->end - span->start);
    if (NSMaxRange(range) > text.length) {
      continue;
    }
    NSMutableDictionary *attributes = [[text attributesAtIndex:range.location effectiveRange:NULL] mutableCopy];
    UIFont *font = attributes[NSFontAttributeName];
    const CGFloat size = (font != nil ? font.pointSize : UIFont.systemFontSize) * kInlineMathScale;
    attributes[NSAttachmentAttributeName] = [[DSMathAttachment alloc] initWithLayout:span->layout
                                                                            fontSize:size
                                                                               color:attributes[NSForegroundColorAttributeName]];
    attributes[DSMathLiteralAttributeName] = [text.string substringWithRange:range];
    [text replaceCharactersInRange:range
              withAttributedString:[[NSAttributedString alloc] initWithString:@"\uFFFC" attributes:attributes]];
  }
}

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
@property (nonatomic) uint32_t containerId;
@property (nonatomic) BOOL writtenOpen;
@property (nonatomic) BOOL summary;
@end

@implementation DSContainerInfo

- (BOOL)isEqual:(id)object {
  if (object == self) {
    return YES;
  }
  if (![object isKindOfClass:[DSContainerInfo class]]) {
    return NO;
  }
  DSContainerInfo *other = object;
  return self.kind == other.kind && self.leading == other.leading && self.width == other.width &&
      self.continuesPrevious == other.continuesPrevious && self.barWidth == other.barWidth &&
       self.markerGap == other.markerGap && self.containerId == other.containerId &&
       self.writtenOpen == other.writtenOpen && self.summary == other.summary &&
      (self.marker == other.marker || [self.marker isEqualToAttributedString:other.marker]) &&
      (self.barColor == other.barColor || [self.barColor isEqual:other.barColor]);
}

- (NSUInteger)hash {
  return (NSUInteger)self.kind;
}
@end

@implementation DSTagStyles

- (BOOL)isEmpty {
  return self.names.count == 0;
}

- (NSDictionary *)styleForTag:(NSString *)name {
  if (name.length == 0 || self.names.count == 0) {
    return nil;
  }
  NSString *normalized = name.lowercaseString;
  for (NSUInteger index = 0; index < self.names.count; index++) {
    if ([self.names[index] isEqualToString:normalized]) {
      const long long color = index < self.colors.count ? self.colors[index].longLongValue : -1;
      const long long flags = index < self.flags.count ? self.flags[index].longLongValue : 0;
      return @{@"color" : @(color), @"flags" : @(flags)};
    }
  }
  return nil;
}

@end

@implementation DSLinkVariants

- (BOOL)isEmpty {
  return self.prefixes.count == 0;
}

- (NSDictionary *)variantForURL:(NSString *)url {
  if (url.length == 0 || self.prefixes.count == 0) {
    return nil;
  }
  NSUInteger at = 0;
  for (NSUInteger variant = 0; variant < self.counts.count; variant++) {
    const NSUInteger count = self.counts[variant].unsignedIntegerValue;
    for (NSUInteger index = 0; index < count && at + index < self.prefixes.count; index++) {
      if ([url hasPrefix:self.prefixes[at + index]]) {
        const long long color =
            variant < self.colors.count ? self.colors[variant].longLongValue : -1;
        const BOOL underline =
            variant < self.underlines.count ? self.underlines[variant].boolValue : NO;
        return @{@"color" : @(color), @"underline" : @(underline)};
      }
    }
    at += count;
  }
  return nil;
}

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

/// Text storage can keep metadata on an unchanged prefix only when every
/// drawing and accessibility field matches the incoming immutable snapshot.
/// Equality also lets TextKit treat old/new metadata as one block's range.
- (BOOL)isEqual:(id)object {
  if (object == self) {
    return YES;
  }
  if (![object isKindOfClass:[DSBlockInfo class]]) {
    return NO;
  }
  DSBlockInfo *other = object;
  return self.index == other.index && self.decoration == other.decoration && self.headingLevel == other.headingLevel &&
      self.spacingBefore == other.spacingBefore && self.contentLeading == other.contentLeading &&
      self.rightToLeft == other.rightToLeft && [self.containers isEqualToArray:other.containers] &&
      (self.rule == other.rule || [self.rule isEqual:other.rule]) &&
      (self.accessibilityPrefix == other.accessibilityPrefix ||
       [self.accessibilityPrefix isEqualToString:other.accessibilityPrefix]);
}

- (NSUInteger)hash {
  return (NSUInteger)self.index;
}
@end

@interface DSDecorationMetrics ()
@property (nonatomic) UIColor *color;
@property (nonatomic) CGFloat height;
@end

@implementation DSDecorationMetrics

- (BOOL)isEqual:(id)object {
  if (object == self) {
    return YES;
  }
  if (![object isKindOfClass:[DSDecorationMetrics class]]) {
    return NO;
  }
  DSDecorationMetrics *other = object;
  return self.height == other.height && (self.color == other.color || [self.color isEqual:other.color]);
}

- (NSUInteger)hash {
  return self.color.hash;
}
@end

/// Keeps a lowered run from lifting its line.
///
/// A paragraph holding a script is capped at its own line height, but TextKit
/// sets a capped line's baseline from the line's deepest descent, so a subscript
/// moved the whole line up toward the block above. The web's `line-height: 0`
/// lets the script hang below instead, which is what this restores: the baseline
/// goes back down by the descent the lowered runs added. Stateless, so one
/// instance serves every stack on every thread.
@interface DSScriptLineDelegate : NSObject <NSLayoutManagerDelegate>
@end

@implementation DSScriptLineDelegate

+ (instancetype)shared {
  static DSScriptLineDelegate *shared;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    shared = [DSScriptLineDelegate new];
  });
  return shared;
}

- (BOOL)layoutManager:(NSLayoutManager *)layoutManager
    shouldSetLineFragmentRect:(inout CGRect *)lineFragmentRect
         lineFragmentUsedRect:(inout CGRect *)lineFragmentUsedRect
               baselineOffset:(inout CGFloat *)baselineOffset
              inTextContainer:(NSTextContainer *)textContainer
                forGlyphRange:(NSRange)glyphRange {
  NSTextStorage *storage = layoutManager.textStorage;
  const NSRange characters = [layoutManager characterRangeForGlyphRange:glyphRange actualGlyphRange:NULL];
  if (characters.length == 0) {
    return NO;
  }
  // Only a capped paragraph: one that is not keeps the height a lowered run
  // asks for, and its baseline is already where the text above put it.
  NSParagraphStyle *style = [storage attribute:NSParagraphStyleAttributeName
                                       atIndex:characters.location
                                effectiveRange:NULL];
  if (style.maximumLineHeight <= 0) {
    return NO;
  }
  __block CGFloat plain = 0;
  __block CGFloat lowered = 0;
  [storage enumerateAttributesInRange:characters
                              options:0
                           usingBlock:^(NSDictionary<NSAttributedStringKey, id> *attributes, NSRange, BOOL *) {
                             UIFont *font = attributes[NSFontAttributeName];
                             if (font == nil) {
                               return;
                             }
                             const CGFloat descent = -font.descender;
                             const CGFloat offset = [attributes[NSBaselineOffsetAttributeName] doubleValue];
                             plain = MAX(plain, descent);
                             lowered = MAX(lowered, descent - offset);
                           }];
  const CGFloat lift = lowered - plain;
  if (lift <= 0) {
    return NO;
  }
  *baselineOffset += lift;
  return YES;
}

@end

/// Compare the rendered characters and attributes, because completing Markdown
/// can restyle characters that were already visible in an earlier snapshot.
static NSUInteger unchangedPrefix(NSAttributedString *before, NSAttributedString *after) {
  const NSUInteger limit = MIN(before.length, after.length);
  NSString *oldText = before.string;
  NSString *newText = after.string;
  NSUInteger prefix = 0;
  unichar oldCharacters[512];
  unichar newCharacters[512];
  while (prefix < limit) {
    const NSUInteger count = MIN((NSUInteger)512, limit - prefix);
    [oldText getCharacters:oldCharacters range:NSMakeRange(prefix, count)];
    [newText getCharacters:newCharacters range:NSMakeRange(prefix, count)];
    NSUInteger matching = 0;
    while (matching < count && oldCharacters[matching] == newCharacters[matching]) {
      matching++;
    }
    prefix += matching;
    if (matching != count) {
      break;
    }
  }
  NSUInteger index = 0;
  while (index < prefix) {
    NSRange oldRange;
    NSRange newRange;
    NSDictionary *oldAttributes = [before attributesAtIndex:index effectiveRange:&oldRange];
    NSDictionary *newAttributes = [after attributesAtIndex:index effectiveRange:&newRange];
    if (![oldAttributes isEqualToDictionary:newAttributes]) {
      return index;
    }
    index = MIN(prefix, MIN(NSMaxRange(oldRange), NSMaxRange(newRange)));
  }
  return prefix;
}

void DSUpdateTextStorage(NSTextStorage *storage, NSAttributedString *text) {
  NSUInteger prefix = unchangedPrefix(storage, text);
  if (prefix == storage.length && prefix == text.length) {
    return;
  }
  if (prefix > 0) {
    // Include the preceding composed character: appending a combining mark or
    // completing an emoji sequence can change its glyph without changing its
    // leading UTF-16 units. Never split a surrogate pair at the edit boundary.
    prefix = MIN([storage.string rangeOfComposedCharacterSequenceAtIndex:prefix - 1].location,
                 [text.string rangeOfComposedCharacterSequenceAtIndex:prefix - 1].location);
  }
  const NSRange replacement = NSMakeRange(prefix, storage.length - prefix);
  NSAttributedString *suffix = [text attributedSubstringFromRange:NSMakeRange(prefix, text.length - prefix)];
  [storage replaceCharactersInRange:replacement withAttributedString:suffix];
}


@implementation DSTextKitStack

- (instancetype)initWithLayoutManager:(NSLayoutManager *)layoutManager {
  if (self = [super init]) {
    _textStorage = [NSTextStorage new];
    _layoutManager = layoutManager;
    _layoutManager.usesFontLeading = YES;
    _layoutManager.delegate = [DSScriptLineDelegate shared];
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
    info.containerId = container.id;
    info.writtenOpen = container.open;
    info.summary = container.showsMarker;
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
@property (nonatomic) uint32_t customContainer;
@property (nonatomic) NSAttributedString *attributedString;
@property (nonatomic) CGFloat topInset;
@property (nonatomic) CGFloat bottomInset;
@property (nonatomic) NSArray<DSBlockInfo *> *blocks;
@property (nonatomic) NSArray<NSValue *> *blockRanges;
@property (nonatomic) DSThemeColors *theme;
@end

@implementation DSTextSegment {
  /// Keep the unwrapped width and recent wrapped widths together, so toggling
  /// wrap or measuring a second width does not discard the first layout.
  struct Measurement {
    CGFloat width;
    CGFloat height;
    CGFloat usedWidth;
  };
  NSLock *_measurementLock;
  std::array<Measurement, 3> _measurements;
  NSUInteger _measurementCount;
  BOOL _allowsTailMeasurementReuse;
}

/// `inCard` is true for the one-block body of a code card: the card draws the
/// background and the padding, and code is laid out left to right however the
/// block's own characters read.
/// The per-URL link styles, or nil. Links are the only runs that read it, so
/// the code card's body and the table's cells do not take one.
+ (instancetype)segmentWithDocument:(const Document &)document
                               from:(size_t)from
                                 to:(size_t)to
                             theme:(const Theme &)theme
                              scale:(CGFloat)scale
                         markerFont:(UIFont *)markerFont
                  syntaxHighlighting:(BOOL)syntaxHighlighting
                             inCard:(BOOL)inCard
                              caret:(const PlacedCaret &)caret
                           variants:(DSLinkVariants *)variants
                          tagStyles:(DSTagStyles *)tagStyles {
  DSTextSegment *segment = [DSTextSegment new];
  segment->_allowsTailMeasurementReuse = !inCard;
  segment->_measurementLock = [NSLock new];
  segment.theme = [DSThemeColors colorsFor:theme];
  DSThemeColors *colors = segment.theme;
  NSMutableAttributedString *text = [NSMutableAttributedString new];
  NSMutableArray<DSBlockInfo *> *blocks = [NSMutableArray arrayWithCapacity:to - from];
  NSMutableArray<NSValue *> *blockRanges = [NSMutableArray arrayWithCapacity:to - from];
  // The last block's own typography, which a caret after it takes. Held here
  // rather than looked up again, because the placeholder below is drawn in the
  // face and color of the character it follows.
  UIFont *lastFont = nil;
  UIColor *lastColor = nil;
  NSParagraphStyle *lastStyle = nil;

  for (size_t index = from; index < to; index++) {
    const Block &block = document.blocks[index];
    const Block *previous = index > from ? &document.blocks[index - 1] : nullptr;
    const bool isCode = block.kind == BlockKind::CodeBlock;

    DSBlockInfo *info = [DSBlockInfo new];
    info.index = static_cast<NSInteger>(index);
    info.headingLevel = block.headingLevel;
    // The engine's own answer, with the `dir` prop already folded into it. A code
    // card's body is the one place it is overridden outright: a fenced block is
    // laid out left to right on both platforms whatever it contains, which is the
    // same rule `Direction.h` states, and saying it here keeps a code card from
    // depending on which of the two the caller was.
    const downstream::BlockDirection resolved =
        isCode && inCard ? downstream::BlockDirection::LeftToRight : block.direction;
    const NSWritingDirection direction =
        resolved == downstream::BlockDirection::Inherit
            ? NSWritingDirectionLeftToRight
            : writingDirectionFor(resolved);
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
    // TextKit grows a line fragment to fit a baseline offset, so a raised or
    // lowered run opens its line by about four points where the web's
    // `line-height: 0` on `sub` and `sup` does not. A paragraph holding a
    // script or a footnote reference is capped at the block's own line height
    // so a formula's marker cannot change the leading of the text around it.
    // A paragraph with an inline formula keeps its natural height: an
    // attachment's box is what the line has to hold.
    if (holdsBaselineShift(block) && !hasInlineMath(block)) {
      // The cap is the height the line would have anyway: the theme's own line
      // height when it set one, and the face's when it did not, which is what
      // the platform would use for itself.
      const CGFloat themed = font.lineHeight * scale;
      style.maximumLineHeight = themed > 0 ? themed : baseFont.lineHeight;
    }
    // Recorded after the cap, so a caret drawn after this block's text carries
    // the same face, color and paragraph style the text itself has.
    lastFont = baseFont;
    lastColor = baseColor;
    lastStyle = style;
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
              static_cast<uint32_t>(block.text.size()), document, variants, tagStyles);
    if (isCode && syntaxHighlighting) {
      applyTokens(text, block, colors, start, 0, static_cast<uint32_t>(block.text.size()));
    }
    replaceInlineMath(text, block, start);
    [blocks addObject:info];
    [blockRanges addObject:[NSValue valueWithRange:NSMakeRange(start, text.length - start - (hasSeparator ? 1 : 0))]];
  }

  // The caret goes after the last block, inside this segment, so it takes the
  // face and color of the character it follows and wraps to the next line when
  // the line is full — the same thing Streamdown's `::after` does.
  if (caret.place == CaretPlace::AfterText && lastFont != nil) {
    NSTextAttachment *attachment = [NSTextAttachment new];
    attachment.image = nil;
    // The bounds stay inside the font's own ascent and descent, so a line
    // holding a caret is exactly as tall as one without it. That is the whole
    // reason the placeholder is in the measured text rather than an overlay: the
    // measured height and the drawn height are then the same number.
    const CGFloat size = lastFont.pointSize;
    attachment.bounds = CGRectMake(0, std::round(size * -0.24), std::round(size * 0.5),
                                   std::round(size * 1.06));
    NSMutableAttributedString *placeholder =
        [[NSMutableAttributedString alloc] initWithString:kCaretPlaceholder
                                              attributes:@{
                                                NSFontAttributeName : lastFont,
                                                NSForegroundColorAttributeName : lastColor,
                                                NSParagraphStyleAttributeName : lastStyle,
                                                DSCaretKindAttributeName : @(static_cast<unsigned char>(caret.kind)),
                                              }];
    [placeholder addAttribute:NSAttachmentAttributeName value:attachment range:NSMakeRange(0, 1)];
    [text appendAttributedString:placeholder];
  }

  segment.attributedString = [text copy];
  segment.blocks = [blocks copy];
  segment.blockRanges = [blockRanges copy];
  // The plan grouped this segment for one custom container, if it did: the
  // first block's innermost Custom frame names it, and every block in the range
  // shares it by construction of the grouping.
  uint32_t custom = UINT32_MAX;
  if (blocks.count > 0) {
    for (DSContainerInfo *frame in ((DSBlockInfo *)blocks[0]).containers) {
      if (frame.kind == DSContainerKindCustom) {
        custom = frame.containerId;
      }
    }
  }
  segment.customContainer = custom;
  return segment;
}

- (BOOL)allowsTailMeasurementReuse {
  return _allowsTailMeasurementReuse;
}

/// Lays the text out once at `width` and hands back the measurements callers
/// need, so height and used width come from the same layout.
static void layoutText(DSTextSegment *segment, NSAttributedString *text, CGFloat width, CGFloat *height,
                       CGFloat *usedWidth) {
  // Worker-local mutable scratch layouts never belong to an immutable
  // snapshot or mounted view. Two exact widths and a text cap bound retention.
  struct Workspace {
    __strong DSTextKitStack *stack = nil;
    CGFloat width = 0;
  };
  static thread_local std::array<Workspace, 2> workspaces;
  DSTextKitStack *stack = nil;
  if ([segment allowsTailMeasurementReuse] && text.length <= 64 * 1024 && std::isfinite(width) && width > 0) {
    size_t index = 0;
    while (index < workspaces.size() && (workspaces[index].stack == nil || workspaces[index].width != width)) {
      index++;
    }
    if (index == workspaces.size()) {
      index = workspaces.size() - 1;
      workspaces[index].stack = [[DSTextKitStack alloc] initWithLayoutManager:[NSLayoutManager new]];
      workspaces[index].width = width;
      workspaces[index].stack.textContainer.size = CGSizeMake(width, CGFLOAT_MAX);
    }
    std::swap(workspaces[0], workspaces[index]);
    stack = workspaces[0].stack;
    DSUpdateTextStorage(stack.textStorage, text);
  } else {
    stack = [[DSTextKitStack alloc] initWithLayoutManager:[NSLayoutManager new]];
    stack.textContainer.size = CGSizeMake(std::isfinite(width) ? width : CGFLOAT_MAX, CGFLOAT_MAX);
    [stack.textStorage setAttributedString:text];
  }
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
  for (NSUInteger index = 0; index < _measurementCount; index++) {
    if (_measurements[index].width == width) {
      const Measurement cached = _measurements[index];
      // Most recently used first. Three values bound both the memory and the
      // lock-held work, even when a view is resized through many widths.
      std::move_backward(_measurements.begin(), _measurements.begin() + index,
                         _measurements.begin() + index + 1);
      _measurements[0] = cached;
      if (height != nullptr) {
        *height = cached.height;
      }
      if (usedWidth != nullptr) {
        *usedWidth = cached.usedWidth;
      }
      [_measurementLock unlock];
      return;
    }
  }
  [_measurementLock unlock];

  CGFloat measuredHeight = 0;
  CGFloat measuredUsedWidth = 0;
  layoutText(self, _attributedString, width, &measuredHeight, &measuredUsedWidth);

  // Two threads can measure the same width at once. The text and the width are
  // the same, so both passes produce the same numbers, and the later write is
  // as good as the first; each caller answers from its own pass either way.
  [_measurementLock lock];
  NSUInteger index = 0;
  while (index < _measurementCount && _measurements[index].width != width) {
    index++;
  }
  if (index == _measurementCount && _measurementCount < _measurements.size()) {
    _measurementCount++;
  }
  index = std::min(index, _measurements.size() - 1);
  std::move_backward(_measurements.begin(), _measurements.begin() + index,
                     _measurements.begin() + index + 1);
  _measurements[0] = {width, measuredHeight, measuredUsedWidth};
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

/// The container bars and markers a box segment draws, and the leading they
/// take, through the same `buildContainers` a text segment uses.
///
/// A formula and an image are boxes rather than prose, but a formula inside a
/// quote still needs the quote's bar beside it and a picture in a list item
/// still needs the item's marker, so both go through the one function that
/// builds them. `previous` is null because a box is always its own segment and
/// therefore never continues a container from the segment above it.
static void buildBoxContainers(const Block &block, UIFont *markerFont, DSThemeColors *colors, const Theme &theme,
                               CGFloat scale, NSMutableArray<DSContainerInfo *> *containers, CGFloat *leading) {
  NSMutableArray<NSString *> *spoken = [NSMutableArray new];
  bool inQuote = false;
  *leading = buildContainers(block, nullptr, theme, colors, scale, markerFont,
                            [colors colorForSlot:kSlotListMarkerColor fallback:UIColor.secondaryLabelColor],
                            containers, spoken, &inQuote);
}

@interface DSMathSegment ()
@property (nonatomic, nullable) MathLayout::Handle layout;
@property (nonatomic) std::string tex;
@property (nonatomic) std::string literal;
@property (nonatomic) std::string error;
@property (nonatomic) NSInteger blockIndex;
@property (nonatomic, copy) NSArray<DSContainerInfo *> *containers;
@property (nonatomic) CGFloat contentLeading;
@property (nonatomic) CGFloat fontSize;
@property (nonatomic) CGFloat padding;
@property (nonatomic) CGFloat marginTop;
@property (nonatomic) CGFloat marginBottom;
@property (nonatomic) CGFloat textAlign;
@property (nonatomic, nullable) UIColor *backgroundColor;
@property (nonatomic, nullable) UIColor *color;
@property (nonatomic, nullable) UIColor *errorColor;
@end

@implementation DSMathSegment

+ (instancetype)segmentWithDocument:(const Document &)document
                               index:(size_t)index
                              theme:(const Theme &)theme
                               scale:(CGFloat)scale
                         markerFont:(UIFont *)markerFont {
  DSMathSegment *segment = [DSMathSegment new];
  const Block &block = document.blocks[index];
  segment.blockIndex = static_cast<NSInteger>(index);
  DSThemeColors *colors = [DSThemeColors colorsFor:theme];
  {
    NSMutableArray<DSContainerInfo *> *containers = [NSMutableArray new];
    CGFloat leading = 0;
    buildBoxContainers(document.blocks[index], markerFont, colors, theme, scale, containers, &leading);
    segment.containers = containers;
    segment.contentLeading = leading;
  }
  // A block's formula covers its whole text, which is the TeX with no delimiters
  // for a fence and the whole literal for a promoted `$$…$$` paragraph. Either
  // way the span says which, and the display list is the only thing drawn.
  if (!block.math.empty()) {
    const MathSpan &span = block.math.front();
    segment.layout = span.layout;
    segment.tex = span.tex;
    // The fallback is the literal a reader saw before math existed, delimiters
    // included, because `Block::text` is exactly that and a formula that cannot
    // be laid out is not a formula that is missing.
    segment.literal = utf16ToUtf8(block.text);
    segment.error = span.error;
  }
  const BlockFont formula = mathFont(theme);
  segment.fontSize = static_cast<CGFloat>(formula.size * scale);
  segment.padding = static_cast<CGFloat>(mathPadding(theme) * scale);
  segment.marginTop = static_cast<CGFloat>(theme_layout::mathMarginTop(theme) * scale);
  segment.marginBottom = static_cast<CGFloat>(theme_layout::mathMarginBottom(theme) * scale);
  segment.textAlign = static_cast<CGFloat>(mathTextAlign(theme));
  // `inherit` is the default and means the block's own color shows through, so
  // these are nil unless a theme named them. A formula with no color of its own
  // is what lets dark mode reach it without a relayout.
  segment.backgroundColor = [colors colorForSlot:kSlotMathBackgroundColor];
  segment.color = [colors colorForSlot:kSlotMathColor];
  segment.errorColor = [colors colorForSlot:kSlotMathErrorColor];
  return segment;
}

- (CGFloat)height {
  if (self.layout == nil) {
    // No layout: the source as a line of text, which is what the reader saw
    // before math existed and what a formula the engine refused shows. A tall
    // fraction is not available here, so the line is the body's own line height
    // rather than a guess at the formula's.
    return self.fontSize * 1.2;
  }
  return mathBlockHeight(*self.layout, self.fontSize, self.padding);
}

- (CGFloat)contentWidth {
  return self.layout != nil ? self.layout->width * self.fontSize : 0;
}

@end

@interface DSImageSegment ()
@property (nonatomic) std::vector<uint32_t> images;
@property (nonatomic) std::vector<std::string> altTexts;
@property (nonatomic) std::vector<std::string> urls;
@property (nonatomic) std::vector<std::string> titles;
@property (nonatomic) std::vector<int32_t> links;
@property (nonatomic) uint32_t imageOrdinal;
@property (nonatomic) NSInteger blockIndex;
@property (nonatomic, copy) NSArray<DSContainerInfo *> *containers;
@property (nonatomic) CGFloat contentLeading;
@property (nonatomic) downstream::ImageMetrics metrics;
@property (nonatomic, nullable) UIColor *placeholderColor;
@end

@implementation DSImageSegment

+ (instancetype)segmentWithDocument:(const Document &)document
                               index:(size_t)index
                              theme:(const Theme &)theme
                               scale:(CGFloat)scale
                         markerFont:(UIFont *)markerFont {
  DSImageSegment *segment = [DSImageSegment new];
  const Block &block = document.blocks[index];
  segment.blockIndex = static_cast<NSInteger>(index);
  segment.images = block.images;
  DSThemeColors *colors = [DSThemeColors colorsFor:theme];
  {
    NSMutableArray<DSContainerInfo *> *containers = [NSMutableArray new];
    CGFloat leading = 0;
    buildBoxContainers(block, markerFont, colors, theme, scale, containers, &leading);
    segment.containers = containers;
    segment.contentLeading = leading;
  }
  std::vector<std::string> altTexts;
  std::vector<std::string> urls;
  std::vector<std::string> titles;
  std::vector<int32_t> links;
  altTexts.reserve(block.images.size());
  urls.reserve(block.images.size());
  titles.reserve(block.images.size());
  links.reserve(block.images.size());
  for (const uint32_t ordinal : block.images) {
    // The destination only when the engine's policy allows it, so a view cannot
    // request what the engine refused; the state already says `Blocked` for it.
    const bool known = ordinal < document.images.size();
    urls.push_back(known && document.images[ordinal].loadable ? document.images[ordinal].url : std::string());
    titles.push_back(known ? document.images[ordinal].title : std::string());
    links.push_back(known ? document.images[ordinal].link : -1);
    // The alt texts, which is the block's own text and what accessibility,
    // export, and the fallback all read. Storing them per image is what lets a
    // view show one image's label under that image rather than one label for
    // the block.
    altTexts.push_back(ordinal < document.images.size() ? utf16ToUtf8(document.images[ordinal].alt) : std::string());
  }
  segment.altTexts = std::move(altTexts);
  segment.urls = std::move(urls);
  segment.titles = std::move(titles);
  segment.links = std::move(links);
  segment.placeholderColor = [colors colorForSlot:kSlotImagePlaceholderColor];
  // The metrics come from the shared rule rather than from a platform copy, and
  // are scaled here once so `imageBox` is handed points rather than unscaled
  // numbers every time it is called.
  downstream::ImageMetrics metrics = imageMetrics(theme);
  metrics.marginTop = static_cast<float>(metrics.marginTop * scale);
  metrics.marginBottom = static_cast<float>(metrics.marginBottom * scale);
  metrics.fallbackTextHeight = static_cast<float>(metrics.fallbackTextHeight * scale);
  segment.metrics = metrics;
  return segment;
}

- (std::vector<const downstream::ImageEntry *>)entriesFor:(const downstream::ImageStateLayout &)answers {
  std::vector<const downstream::ImageEntry *> found;
  found.reserve(self.images.size());
  for (const uint32_t ordinal : self.images) {
    // A view handed a document it has no answer for still draws, so a missing
    // entry is a null the caller reads as a placeholder rather than a crash.
    found.push_back(answers.find(ordinal));
  }
  return found;
}

- (downstream::ImageBox)boxAtIndex:(NSUInteger)index
                            column:(CGFloat)column
                           answers:(const downstream::ImageStateLayout &)answers {
  if (index >= self.images.size()) {
    return imageBox(nullptr, static_cast<float>(column), self.metrics);
  }
  return imageBox(answers.find(self.images[index]), static_cast<float>(column), self.metrics);
}

- (CGFloat)heightForColumn:(CGFloat)column answers:(const downstream::ImageStateLayout &)answers {
  if (!answers.enabled) {
    // Images off: one line of alt text for the whole block, which is what
    // turning images off means to a reader.
    return self.metrics.fallbackTextHeight * self.altTexts.size();
  }
  CGFloat height = 0;
  for (NSUInteger index = 0; index < self.images.size(); index++) {
    // Each image is its own box, stacked vertically. The gap between them is the
    // theme's own margin, so a block of three charts is not three boxes but
    // touching as one column.
    height += [self boxAtIndex:index column:column answers:answers].height;
  }
  return height;
}

@end

@interface DSCodeSegment ()
@property (nonatomic) BOOL rightToLeft;
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

@implementation DSCodeSegment {
  std::vector<uint32_t> _nonblankLineStarts;
}

+ (instancetype)segmentWithDocument:(const Document &)document
                               index:(size_t)index
                             theme:(const Theme &)theme
                              scale:(CGFloat)scale
                         markerFont:(UIFont *)markerFont
                  syntaxHighlighting:(BOOL)syntaxHighlighting
                              caret:(const PlacedCaret &)caret {
  DSCodeSegment *segment = [DSCodeSegment new];
  const Block &block = document.blocks[index];
  // The body is a one-block text segment, so a caret inside an open fence goes
  // where a caret after prose would: inside this segment's text, after its last
  // character.
  segment.text = [DSTextSegment segmentWithDocument:document
                                              from:index
                                                to:index + 1
                                             theme:theme
                                              scale:scale
                                        markerFont:markerFont
                                 syntaxHighlighting:syntaxHighlighting
                                            inCard:YES
                                             caret:caret
                                          variants:nil tagStyles:nil];
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
  NSString *const codeText = segment.text.attributedString.string;
  for (uint32_t offset : *segment.lineStarts) {
    if (offset < codeText.length && [codeText characterAtIndex:offset] != '\n') {
      segment->_nonblankLineStarts.push_back(offset);
    }
  }
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
  buildContainers(block, nullptr, theme, segment.theme, scale, markerFont,
                 [segment.theme colorForSlot:kSlotListMarkerColor fallback:UIColor.secondaryLabelColor],
                 containers, spoken, &inQuote);
  // The engine's own sum of the container indents, rather than the one
  // `buildContainers` accumulated on the way past. A React child replacing this
  // block is measured at this number by the shadow node, so the two platforms
  // and the node cannot drift apart on where the column is.
  segment.contentLeading = theme_layout::contentLeading(theme, block.containers, scale);
  // The engine's own answer again, and for the same reason: a segment's own
  // direction is its first block's, because the segment draws as one text layout
  // and the platform cannot be told otherwise.
  segment.rightToLeft = block.direction == downstream::BlockDirection::RightToLeft;
  segment.containers = containers;
  const size_t newline = block.text.find(u'\n');
  segment.firstLine = toNSString(block.text.substr(0, newline == std::u16string::npos ? block.text.size() : newline));
  return segment;
}

- (CGFloat)headerHeight {
  return theme_layout::kChromeRowHeight;
}

- (NSUInteger)baselineCharacterOffsetAtOrAfter:(NSUInteger)offset {
  const auto line = std::lower_bound(_nonblankLineStarts.begin(), _nonblankLineStarts.end(), offset);
  return line == _nonblankLineStarts.end() ? NSNotFound : *line;
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
@property (nonatomic) int32_t tableOrdinal;
@property (nonatomic) NSString *firstRowText;
@property (nonatomic) NSArray<DSContainerInfo *> *containers;
@property (nonatomic) NSArray<NSNumber *> *columnWidths;
@property (nonatomic) NSArray<NSNumber *> *rowHeights;
@property (nonatomic) uint32_t rowCount;
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

/// Table cells can hold styled tag runs, so the table builder takes the same
/// styling inputs as the text builder: the document for names, and the two
/// objects that say how links and tags draw.
+ (instancetype)segmentWithDocument:(const Document &)document
                               from:(size_t)from
                                 to:(size_t)to
                             theme:(const Theme &)theme
                              scale:(CGFloat)scale
                         markerFont:(UIFont *)markerFont
                           variants:(DSLinkVariants *)variants
                          tagStyles:(DSTagStyles *)tagStyles {
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
  // A table draws as one grid, so its direction is the first row's — and the
  // engine has already decided that one.
  segment.rightToLeft = first.direction == downstream::BlockDirection::RightToLeft;
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
                cell.end, document, variants, tagStyles);
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
  // Read once, at build time, because the cap is counted in rows and the
  // measurement runs on a worker thread where touching a collection later would
  // be a data race against the next snapshot's own build.
  segment.rowCount = static_cast<uint32_t>(rows.count);
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

// Approximate metadata/tail cost, following the segment cache's text weighting.
// Shared committed payloads are omitted; renderings can retain them even after
// the separate committed segment cache evicts them.
static constexpr NSUInteger kRenderedDocumentCostLimit = 8 * 1024 * 1024;

static NSUInteger addingRenderingCost(NSUInteger cost, size_t characters, NSUInteger metadata) {
  // Only the budget comparison matters above this value. Saturation keeps a
  // pathological source length from wrapping the estimate back to a small cost.
  constexpr NSUInteger cap = kRenderedDocumentCostLimit + 1;
  if (cost >= cap || metadata >= cap - cost) {
    return cap;
  }
  const NSUInteger remaining = cap - cost - metadata;
  return characters > remaining / 16 ? cap : cost + metadata + characters * 16;
}

/// NSCache may discard an object larger than its budget immediately. Keep only
/// the latest such rendering reusable until the next insertion, which releases
/// that slot's previous owner.
@interface DSDocumentRenderingCache : NSObject
- (DSRenderedDocument *)objectForKey:(NSString *)key;
- (void)setObject:(DSRenderedDocument *)object forKey:(NSString *)key cost:(NSUInteger)cost;
- (void)removeAllObjects;
@end

@implementation DSDocumentRenderingCache {
  NSCache<NSString *, DSRenderedDocument *> *_cache;
  NSLock *_lock;
  NSString *_latestOversizedKey;
  DSRenderedDocument *_latestOversized;
}

- (instancetype)init {
  if ((self = [super init])) {
    _cache = [NSCache new];
    _cache.countLimit = 128;
    _cache.totalCostLimit = kRenderedDocumentCostLimit;
    _lock = [NSLock new];
  }
  return self;
}

- (DSRenderedDocument *)objectForKey:(NSString *)key {
  [_lock lock];
  DSRenderedDocument *value = [_latestOversizedKey isEqualToString:key] ? _latestOversized : [_cache objectForKey:key];
  [_lock unlock];
  return value;
}

- (void)setObject:(DSRenderedDocument *)object forKey:(NSString *)key cost:(NSUInteger)cost {
  [_lock lock];
  _latestOversizedKey = nil;
  _latestOversized = nil;
  if (cost > kRenderedDocumentCostLimit) {
    _cache.countLimit = 127;
    [_cache removeObjectForKey:key];
    _latestOversizedKey = [key copy];
    _latestOversized = object;
  } else {
    _cache.countLimit = 128;
    [_cache setObject:object forKey:key cost:cost];
  }
  [_lock unlock];
}

- (void)removeAllObjects {
  [_lock lock];
  _latestOversizedKey = nil;
  _latestOversized = nil;
  [_cache removeAllObjects];
  _cache.countLimit = 128;
  [_lock unlock];
}
@end

@interface DSRenderedDocument ()
@property (nonatomic) DSThemeColors *theme;
@end

@implementation DSRenderedDocument {
  NSArray *_segments;
  NSArray<NSNumber *> *_segmentBlockIndexes;
  uint32_t _settledBlocks;
  /// The caret this rendering reserved room for, which the view draws at its
  /// placeholder. Already resolved, because the placeholder's presence is a height
  /// and the measurement had to agree about it.
  PlacedCaret _caret;
  std::vector<CGFloat> _gaps;
  NSLock *_lock;
  CGFloat _cachedWidth;
  uint32_t _cachedChrome;
  /// The code layout the cached frames were measured under, so a measurement is
  /// only reused for the layout it was made with.
  downstream::CodeLayout _cachedCode;
  /// The image answers the cached frames were stacked with. Part of the key
  /// because a load changes a picture's height, so a frame cached for a
  /// placeholder is wrong for a loaded image.
  downstream::ImageStateLayout _cachedImages;
  /// The hosted children's heights the cached frames were stacked with. Part of
  /// the key for the reason the image answers are: a chart is as tall as the
  /// component that draws it, so a frame cached for one component's height is
  /// wrong for another's.
  downstream::HostLayout _cachedHosts;
  /// The table layout the cached frames were built at, for the same reason
  /// `_cachedCode` is: a capped table and an expanded one are different heights.
  downstream::TableLayout _cachedTables;
  /// The reader's own choices, which the cache is keyed on for the same reason
  /// the code layout is: a closed and an open body are different heights, so they
  /// are different measurements.
  downstream::ReaderChoices _cachedChoices;
  /// Per-URL link styles, which the text builder reads when it colors a link run.
  DSLinkVariants *_variants;
  /// Styles for app-allowed inline tags, read when a run names one. Beside
  /// `_variants` because the two answer different questions about the same
  /// runs.
  DSTagStyles *_tagStyles;
  bool _hasCachedFrames;
  /// Cache native geometry. Measurement needs no NSValue boxing, and each
  /// caller copies only the frames it consumes while holding the cache lock.
  DSStackedLayout _cachedLayout;
  NSUInteger _cacheCost;
}

+ (DSDocumentRenderingCache *)cache {
  static DSDocumentRenderingCache *cache;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    // Worker threads prepare every document in a commit before Fabric
    // measures them, so this holds at least a screenful of messages.
    cache = [DSDocumentRenderingCache new];
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

/// The variants folded into a committed segment's cache key.
///
/// Link colors are baked into a segment's text, so two variant sets are two
/// different segments even when everything else is the same.
static NSString *committedVariantsKey(DSLinkVariants *variants) {
  return variantsKey(variants);
}

+ (id)committedSegmentFor:(const Document &)document
                     kind:(SegmentKind)kind
                     from:(size_t)from
                       to:(size_t)to
                    theme:(const Theme &)theme
                    scale:(CGFloat)scale
               markerFont:(UIFont *)markerFont
        syntaxHighlighting:(BOOL)syntaxHighlighting
                  caretAfter:(const PlacedCaret &)caretAfter
                  variants:(DSLinkVariants *)variants
                 tagStyles:(DSTagStyles *)tagStyles {
  // Token colors only affect code. Keep shared prose/table/image segments
  // reusable when a view toggles highlighting on a document containing code.
  syntaxHighlighting = kind == SegmentKind::Code ? syntaxHighlighting : NO;
  // The theme is in the key because a segment holds resolved colors and
  // measured sizes, and a second theme must not read the first one's.
  // The plan's kind is in it for the same reason: a math block and an image-only
  // paragraph are their own segments, so the same identity can build a formula
  // and a paragraph and the two are not interchangeable.
  // The caret is not in it because its placeholder is only ever in the last
  // segment's measured text, and a committed segment is never the last one.
  NSString *key =
      [NSString stringWithFormat:@"%llu:%zu:%llu:%.4f:%d:%d:%@:%@", (unsigned long long)document.blocks[from].identity,
                                 to - from, (unsigned long long)theme.id(), scale, syntaxHighlighting,
                                 static_cast<int>(kind), committedVariantsKey(variants), tagStylesKey(tagStyles)];
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
  // The plan's kind rather than the block's: a math block and an image-only
  // paragraph are their own segments, and building them as text would show a
  // committed formula as its source and a committed picture as its alt text.
  switch (kind) {
    case SegmentKind::Math:
      cached->segment = (id)[DSMathSegment segmentWithDocument:document
                                                         index:from
                                                        theme:theme
                                                        scale:scale
                                                   markerFont:markerFont];
      break;
    case SegmentKind::Image:
      cached->segment = (id)[DSImageSegment segmentWithDocument:document
                                                          index:from
                                                         theme:theme
                                                          scale:scale
                                                    markerFont:markerFont];
      break;
    case SegmentKind::Table:
      cached->segment = (id)[DSTableSegment segmentWithDocument:document
                                                            from:from
                                                              to:to
                                                           theme:theme
                                                           scale:scale
                                                      markerFont:markerFont
                                                        variants:variants
                                                       tagStyles:tagStyles];
      break;
    case SegmentKind::Code:
      cached->segment = (id)[DSCodeSegment segmentWithDocument:document
                                                         index:from
                                                        theme:theme
                                                        scale:scale
                                                   markerFont:markerFont
                                        syntaxHighlighting:syntaxHighlighting
                                                         caret:caretAfter];
      break;
    case SegmentKind::Custom:
      // A custom container draws as text until it is hosted: the blocks are
      // ordinary paragraphs, and only a hosted tag replaces them. Falls through
      // to the text builder, because there is nothing custom about unhosted
      // text.
    case SegmentKind::Text:
      cached->segment = (id)[DSTextSegment segmentWithDocument:document
                                                           from:from
                                                             to:to
                                                          theme:theme
                                                           scale:scale
                                                     markerFont:markerFont
                                          syntaxHighlighting:syntaxHighlighting
                                                         inCard:NO
                                                          caret:caretAfter
                                                       variants:variants tagStyles:tagStyles];
      break;
  }
  [self.committedSegmentCache setObject:cached forKey:key cost:cost];
  return cached->segment;
}

/// A cache key for the variants, or empty when there are none to key on.
///
/// The variants change link colors but no heights, so the key only has to tell
/// two variant sets apart — not to be readable. A hash of the arrays is what
/// does that without putting every prefix in the key.
static NSString *variantsKey(DSLinkVariants *variants) {
  if (variants == nil || variants.isEmpty) {
    return @"";
  }
  NSUInteger hash = variants.prefixes.count;
  for (NSString *prefix in variants.prefixes) {
    hash = hash * 31 + prefix.hash;
  }
  for (NSNumber *color in variants.colors) {
    hash = hash * 31 + color.unsignedIntegerValue;
  }
  return [NSString stringWithFormat:@"%lu", (unsigned long)hash];
}

+ (instancetype)renderedDocumentFor:(const DocumentHandle &)document
                          fontScale:(CGFloat)fontScale
                 syntaxHighlighting:(BOOL)syntaxHighlighting
                              theme:(const Theme::Handle &)themeHandle
                             caret:(const PlacedCaret &)caret {
  return [self renderedDocumentFor:document
                         fontScale:fontScale
                syntaxHighlighting:syntaxHighlighting
                             theme:themeHandle
                            caret:caret
                          variants:nil tagStyles:nil];
}

/// A cache key for the tag styles, or empty when there are none to key on.
static NSString *tagStylesKey(DSTagStyles *styles) {
  if (styles == nil || styles.isEmpty) {
    return @"";
  }
  NSUInteger hash = styles.names.count;
  for (NSString *name in styles.names) {
    hash = hash * 31 + name.hash;
  }
  for (NSNumber *color in styles.colors) {
    hash = hash * 31 + color.unsignedIntegerValue;
  }
  return [NSString stringWithFormat:@"%lu", (unsigned long)hash];
}

+ (instancetype)renderedDocumentFor:(const DocumentHandle &)document
                          fontScale:(CGFloat)fontScale
                 syntaxHighlighting:(BOOL)syntaxHighlighting
                              theme:(const Theme::Handle &)themeHandle
                             caret:(const PlacedCaret &)caret
                          variants:(DSLinkVariants *)variants
                         tagStyles:(DSTagStyles *)tagStyles {
  if (!document) {
    return [[self alloc] initWithDocument:document
                               fontScale:fontScale
                      syntaxHighlighting:syntaxHighlighting
                                   theme:themeHandle ? themeHandle : Theme::defaultTheme()
                                  caret:caret
                               variants:variants tagStyles:tagStyles];
  }
  // Preparation requests highlighting, while mounting uses the view prop.
  // A code-free document paints identically under either setting, so it can
  // share the prepared rendering and its exact-width measurements.
  if (!syntaxHighlighting) {
    syntaxHighlighting = std::none_of(document->blocks.begin(), document->blocks.end(),
                                    [](const Block &block) { return block.kind == BlockKind::CodeBlock; });
  }
  // The theme id is in the key rather than the theme itself, because interned
  // themes are stable for the life of a process and their id is all a cache key
  // needs to tell two of them apart. The caret is in it for the same reason: its
  // placeholder is in the measured text, so two renderings that differ only in
  // whether a caret was reserved are two different heights.
  NSString *key =
      [NSString stringWithFormat:@"%llu:%.4f:%d:%llu:%d:%d:%@:%@", (unsigned long long)document->id,
                                 fontScale, syntaxHighlighting,
                                 (unsigned long long)(themeHandle ? themeHandle->id()
                                                                  : Theme::defaultTheme()->id()),
                                 static_cast<int>(caret.kind), static_cast<int>(caret.place),
                                 variantsKey(variants), tagStylesKey(tagStyles)];
  DSRenderedDocument *rendered = [self.cache objectForKey:key];
  if (rendered == nil) {
    rendered = [[self alloc] initWithDocument:document
                                     fontScale:fontScale
                            syntaxHighlighting:syntaxHighlighting
                                      theme:themeHandle ? themeHandle : Theme::defaultTheme()
                                       caret:caret
                                   variants:variants tagStyles:tagStyles];
    [self.cache setObject:rendered forKey:key cost:rendered->_cacheCost];
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
                            theme:(const Theme::Handle &)themeHandle
                           caret:(const PlacedCaret &)caret
                        variants:(DSLinkVariants *)variants
                       tagStyles:(DSTagStyles *)tagStyles {
  if (!(self = [super init])) {
    return nil;
  }
  _lock = [NSLock new];
  _cachedWidth = -1;
  _caret = caret;
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
  _variants = variants;
  _tagStyles = tagStyles;
  _cacheCost = 1024;

  for (const SegmentPlan &segment : planSegments(document)) {
    // Arrays, block indexes, gaps and the cached frame are snapshot-owned even
    // when the segment's attributed text is shared with earlier snapshots.
    _cacheCost = addingRenderingCost(_cacheCost, 0, 128);
    if (!segment.committed) {
      for (size_t index = segment.from; index < segment.to; ++index) {
        _cacheCost = addingRenderingCost(_cacheCost, document.blocks[index].text.size(), 256);
      }
    }
    // The caret belongs to the last segment, whichever kind it is, because that is
    // where the document ends. The plan is in order, so that is the one segment
    // that reaches the last block.
    const PlacedCaret here = segment.to == document.blocks.size() ? caret : PlacedCaret{};
    id built = nil;
    if (segment.committed) {
      built = [DSRenderedDocument committedSegmentFor:document
                                                 kind:segment.kind
                                                 from:segment.from
                                                   to:segment.to
                                                theme:theme
                                                scale:scale
                                           markerFont:markerFont
                                    syntaxHighlighting:syntaxHighlighting
                                           caretAfter:here
                                             variants:_variants tagStyles:_tagStyles];
    } else if (segment.isCode()) {
      built = [DSCodeSegment segmentWithDocument:document
                                           index:segment.from
                                          theme:theme
                                           scale:scale
                                     markerFont:markerFont
                              syntaxHighlighting:syntaxHighlighting
                                           caret:here];
    } else if (segment.isMath()) {
      built = [DSMathSegment segmentWithDocument:document
                                          index:segment.from
                                         theme:theme
                                          scale:scale
                                    markerFont:markerFont];
    } else if (segment.isImage()) {
      built = [DSImageSegment segmentWithDocument:document
                                            index:segment.from
                                           theme:theme
                                            scale:scale
                                      markerFont:markerFont];
    } else if (segment.isTable()) {
      built = [DSTableSegment segmentWithDocument:document
                                             from:segment.from
                                               to:segment.to
                                            theme:theme
                                            scale:scale
                                       markerFont:markerFont
                                         variants:_variants
                                        tagStyles:_tagStyles];
    } else {
      built = [DSTextSegment segmentWithDocument:document
                                            from:segment.from
                                              to:segment.to
                                           theme:theme
                                            scale:scale
                                       markerFont:markerFont
                                syntaxHighlighting:syntaxHighlighting
                                           inCard:NO
                                            caret:here
                                         variants:_variants tagStyles:_tagStyles];
    }
    // The plan counted the code blocks, so the ordinal comes from there rather
    // than from a walk of the blocks. It is assigned here so a committed segment
    // taken from the cache gets it too, and cannot carry a stale one.
    if ([built isKindOfClass:DSTableSegment.class] && segment.tableOrdinal >= 0) {
      ((DSTableSegment *)built).tableOrdinal = segment.tableOrdinal;
    }
    if ([built isKindOfClass:DSCodeSegment.class] && segment.codeOrdinal >= 0) {
      ((DSCodeSegment *)built).codeOrdinal = static_cast<uint32_t>(segment.codeOrdinal);
    }
    // The same for an image block, whose ordinal is the plan's count of the
    // images it holds. Assigned here rather than at build time so a committed
    // segment taken from the cache gets it too, and cannot carry a stale one.
    if ([built isKindOfClass:DSImageSegment.class] && segment.imageOrdinal >= 0) {
      ((DSImageSegment *)built).imageOrdinal = static_cast<uint32_t>(segment.imageOrdinal);
    }
    [segments addObject:built];
    [indexes addObject:@((NSInteger)segment.from)];
    _gaps.push_back(segment.from == 0 ? 0 : blockGap(theme, document.blocks[segment.from].spacingBefore,
                                                     &document.blocks[segment.from - 1],
                                                     document.blocks[segment.from]) * scale);
  }
  // A caret that takes a line of its own is a whole extra segment, because that is
  // what it is: room for the next block's first line. It replaces nothing, so when
  // text arrives it is simply gone and the block that arrives has the same gap
  // above it.
  if (caret.place == CaretPlace::OwnLine) {
    _cacheCost = addingRenderingCost(_cacheCost, 0, 384);
    [segments addObject:[DSRenderedDocument ownLineCaretSegmentWithTheme:theme
                                                                  scale:scale
                                                             markerFont:markerFont
                                                                  caret:caret]];
    [indexes addObject:@(document.blocks.size())];
    _gaps.push_back(0);
  }
  _segments = [segments copy];
  _segmentBlockIndexes = [indexes copy];
  return self;
}

- (NSArray *)segments {
  return _segments;
}

/// A one-line text segment holding nothing but a caret.
///
/// The own-line case: after a table, a closed fence, or a rule, the next block
/// will start on a fresh line, so the caret takes that line rather than sitting at
/// the end of a finished one. When text arrives this segment is simply gone, and
/// the block that replaces it starts on the line the caret already occupied, so
/// nothing jumps.
+ (DSTextSegment *)ownLineCaretSegmentWithTheme:(const Theme &)theme
                                           scale:(CGFloat)scale
                                      markerFont:(UIFont *)markerFont
                                           caret:(const PlacedCaret &)caret {
  Document document;
  document.blocks.tail().push_back(Block{});
  return [DSTextSegment segmentWithDocument:document
                                       from:0
                                         to:1
                                      theme:theme
                                       scale:scale
                                  markerFont:markerFont
                           syntaxHighlighting:NO
                                      inCard:NO
                                       caret:caret
                                    variants:nil tagStyles:nil];
}

- (NSArray<NSNumber *> *)segmentBlockIndexes {
  return _segmentBlockIndexes;
}

- (uint32_t)settledBlocks {
  return _settledBlocks;
}

- (PlacedCaret)caret {
  return _caret;
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
                code:(const downstream::CodeLayout &)code
              images:(const downstream::ImageStateLayout &)images
               hosts:(const downstream::HostLayout &)hosts
              tables:(const downstream::TableLayout &)tables
             choices:(const downstream::ReaderChoices &)choices {
  NSArray<DSContainerInfo *> *containers = [segment isKindOfClass:DSTextSegment.class]
      ? ((DSBlockInfo *)((DSTextSegment *)segment).blocks.firstObject).containers
      : [segment containers];
  for (DSContainerInfo *frame in containers) {
    if (frame.kind == DSContainerKindDetails && !frame.summary &&
        !downstream::resolveDetails(choices, frame.containerId, frame.writtenOpen).open) {
      return 0;
    }
  }
  if ([segment isKindOfClass:DSTextSegment.class]) {
    DSTextSegment *text = (DSTextSegment *)segment;
    // A hosted tag replaces its whole segment with the slot: the component
    // owns its height, so the text underneath takes none. Only a segment the
    // plan grouped for one container is ever hosted, which is what makes the
    // container id on the plan the key rather than a block range.
    const uint32_t custom = [text customContainer];
    if (custom != UINT32_MAX && hosts.hostsTag(custom)) {
      return hosts.tagHeightFor(custom);
    }
    return [text heightForWidth:width];
  }
  if ([segment isKindOfClass:DSMathSegment.class]) {
    DSMathSegment *formula = segment;
    // `mathBlockHeight` is the engine's one definition, so a measured formula
    // and a drawn one cannot be different sizes. The margins are the document
    // stack's gap, not the segment's, so they are not added here.
    return [formula height];
  }
  if ([segment isKindOfClass:DSImageSegment.class]) {
    DSImageSegment *pictures = segment;
    // One call into `imageBox` per image, which is the engine's one definition.
    // A load that finishes changes an entry, this returns a different height, and
    // the frame cache — which keys on the answers — rebuilds.
    return [pictures heightForColumn:width - pictures.contentLeading answers:images];
  }
  if ([segment isKindOfClass:DSCodeSegment.class]) {
    DSCodeSegment *card = segment;
    // A hosted fence is as tall as the React child that replaced it: the
    // component owns its height, and the block's margins are a code block's
    // margins, which is why the space around it does not change when a fence
    // becomes a chart.
    if (hosts.hosts(card.codeOrdinal)) {
      return hosts.heightFor(card.codeOrdinal);
    }
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
  // The one definition: the cap, the reader's per-table choice, and the row count
  // all meet in `tableHeight`, which is what a measured table and a drawn table
  // both call. A table taller than the cap is drawn in its own scroll view, so
  // the frame is the cap rather than the grid.
  const CGFloat toolbar = (chrome & kChromeTableToolbar) != 0 ? theme_layout::kChromeRowHeight : 0;
  return tableHeight(resolveTable(tables, table.tableOrdinal), table.rowCount, toolbar);
}

/// The width an unconstrained measure takes: the widest thing that does not
/// fit a view, which is a table's columns or a code line that scrolls.
- (CGFloat)unconstrainedWidthOf:(id)segment
                        atWidth:(CGFloat)width
                           code:(const downstream::CodeLayout &)code {
  if ([segment isKindOfClass:DSMathSegment.class]) {
    // A wide formula is what scrolls sideways, so its own em width at this size
    // is the width an unconstrained measure has to report.
    DSMathSegment *formula = segment;
    return formula.contentLeading + [formula contentWidth] + 2 * formula.padding;
  }
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
                                        code:(const downstream::CodeLayout &)code
                                      images:(const downstream::ImageStateLayout &)images
                                       hosts:(const downstream::HostLayout &)hosts
                                      tables:(const downstream::TableLayout &)tables {
  NSMutableArray<NSValue *> *frames = [NSMutableArray arrayWithCapacity:_segments.count];
  for (const CGRect frame : [self stackForWidth:width chrome:chrome code:code images:images hosts:hosts
                                    tables:tables choices:code.choices includeFrames:YES
                                   includeHosts:NO].frames) {
    [frames addObject:[NSValue valueWithCGRect:frame]];
  }
  return frames;
}

- (CGSize)sizeForWidth:(CGFloat)width
                chrome:(uint32_t)chrome
                   code:(const downstream::CodeLayout &)code
                 images:(const downstream::ImageStateLayout &)images
                  hosts:(const downstream::HostLayout &)hosts
                 tables:(const downstream::TableLayout &)tables {
  return [self stackForWidth:width chrome:chrome code:code images:images hosts:hosts tables:tables
               choices:code.choices includeFrames:NO includeHosts:NO].size;
}

- (DSMeasurement)measureForWidth:(CGFloat)width
                          chrome:(uint32_t)chrome
                             code:(const downstream::CodeLayout &)code
                          images:(const downstream::ImageStateLayout &)images
                           hosts:(const downstream::HostLayout &)hosts
                          tables:(const downstream::TableLayout &)tables {
  DSMeasurement measurement;
  DSStackedLayout stacked = [self stackForWidth:width chrome:chrome code:code images:images hosts:hosts tables:tables
                                      choices:code.choices
                                 includeFrames:NO includeHosts:YES];
  measurement.size = stacked.size;
  measurement.hosts = std::move(stacked.hosts);
  measurement.tagHosts = std::move(stacked.tagHosts);
  return measurement;
}

/// The one stacking pass: every segment's frame, the document's size, and the
/// content rectangle of each hosted fence.
///
/// A measurement only holds for the layout it was made under, so the whole code
/// layout, the image answers, and the hosted heights are all part of the key. A
/// renderer that changes size changes one of them, which is what makes the cache
/// miss rather than leave a chart in the box it was measured at. All three are
/// empty or trivial in every document that registers no renderers, so this is a
/// few comparisons in practice.
- (DSStackedLayout)stackForWidth:(CGFloat)width
                          chrome:(uint32_t)chrome
                             code:(const downstream::CodeLayout &)code
                          images:(const downstream::ImageStateLayout &)images
                           hosts:(const downstream::HostLayout &)hosts
                          tables:(const downstream::TableLayout &)tables
                         choices:(const downstream::ReaderChoices &)choices
                   includeFrames:(BOOL)includeFrames
                    includeHosts:(BOOL)includeHosts {
  [_lock lock];
  if (_hasCachedFrames && _cachedWidth == width && _cachedChrome == chrome && _cachedCode == code &&
      _cachedImages == images && _cachedHosts == hosts && _cachedTables == tables &&
      _cachedChoices == choices) {
    DSStackedLayout cached;
    if (includeFrames) {
      cached.frames = _cachedLayout.frames;
    }
    if (includeHosts) {
      cached.hosts = _cachedLayout.hosts;
      cached.tagHosts = _cachedLayout.tagHosts;
    }
    cached.size = _cachedLayout.size;
    [_lock unlock];
    return cached;
  }
  [_lock unlock];

  DSStackedLayout stacked;
  stacked.frames.reserve(_segments.count);
  stacked.hosts.reserve(hosts.entries.size());
  stacked.tagHosts.reserve(hosts.tags.size());
  // Segment edges round up from their exact positions, so frames stay on
  // whole points without rounding error accumulating across segments.
  CGFloat y = 0;
  CGFloat unconstrainedWidth = 0;
  const BOOL constrained = std::isfinite(width);
  for (NSUInteger index = 0; index < _segments.count; index++) {
    id segment = _segments[index];
    const CGFloat height =
        [self heightOf:segment atWidth:width chrome:chrome code:code images:images hosts:hosts tables:tables
               choices:choices];
    if (height > 0) {
      y += _gaps[index];
    }
    if (!constrained && height > 0) {
      unconstrainedWidth = std::max(unconstrainedWidth, [self unconstrainedWidthOf:segment
                                                                             atWidth:width
                                                                                code:code]);
    }
    const CGFloat top = std::ceil(y);
    // Never shorter than the segment. Rounding both edges up loses up to a
    // point when y is fractional, and a text view lays out only the lines that
    // fit its frame, so a segment a fraction short would drop its last line.
    const CGFloat bottom = std::max(std::ceil(y + height), top + std::ceil(height));
    stacked.frames.push_back(CGRectMake(0, top, constrained ? width : 0, bottom - top));
    // A hosted block's rectangle: its content column, at the full height the
    // component measured. The column comes from the block's own container path
    // through the engine's `contentLeading`, the same number the shadow node
    // measured the child at, so the two cannot disagree about where it goes.
    if ([segment isKindOfClass:DSTextSegment.class]) {
      DSTextSegment *text = segment;
      // A hosted tag's rectangle: its content column, at the full height the
      // component measured. Same shape as a hosted fence's, keyed by container
      // id rather than code ordinal.
      const uint32_t custom = text.customContainer;
      if (custom != UINT32_MAX && hosts.hostsTag(custom)) {
        // The container's own column, from the segment's first block: the slot
        // child is the container, so it starts where the container's text would.
        DSBlockInfo *first = (DSBlockInfo *)text.blocks.firstObject;
        const CGFloat leading = first != nil ? first.contentLeading : 0;
        const CGFloat column = constrained ? std::max<CGFloat>(0, width - leading) : 0;
        stacked.tagHosts.emplace_back(custom, CGRectMake(leading, top, column, bottom - top));
      }
    }
    if ([segment isKindOfClass:DSCodeSegment.class]) {
      DSCodeSegment *card = segment;
      if (hosts.hosts(card.codeOrdinal)) {
        const CGFloat leading = card.contentLeading;
        const CGFloat column = constrained ? std::max<CGFloat>(0, width - leading) : 0;
        CGFloat x = leading;
        if (card.rightToLeft && constrained) {
          // RTL mirrors the indent to the trailing edge, the same way the
          // container bars and the marker are mirrored.
          x = width - leading - column;
        }
        stacked.hosts.emplace_back(card.codeOrdinal,
                                CGRectMake(x, top, column, bottom - top));
      }
    }
    y += height;
  }

  // Every segment spans the view; an unconstrained measure uses the widest
  // segment that does not fit.
  if (!constrained) {
    for (CGRect &frame : stacked.frames) {
      frame.size.width = unconstrainedWidth;
    }
  }
  if (!stacked.frames.empty()) {
    const CGRect last = stacked.frames.back();
    stacked.size = CGSizeMake(std::ceil(CGRectGetWidth(last)), std::ceil(CGRectGetMaxY(last)));
  }

  [_lock lock];
  _cachedWidth = width;
  _cachedChrome = chrome;
  _cachedCode = code;
  _cachedImages = images;
  _cachedHosts = hosts;
  _cachedTables = tables;
  _cachedChoices = choices;
  _hasCachedFrames = YES;
  _cachedLayout = stacked;
  [_lock unlock];

  if (!includeFrames) {
    stacked.frames.clear();
  }
  if (!includeHosts) {
    stacked.hosts.clear();
  }
  return stacked;
}

@end
