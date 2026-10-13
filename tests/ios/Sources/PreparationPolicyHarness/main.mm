#import <UIKit/UIKit.h>
#import <objc/runtime.h>
#import "DSRenderedDocument.h"
#include "RenderPreparer.h"
#include <cstdio>
#include <cstdlib>

// These stand-ins allow the production preparation and measurement methods to
// execute without React's bridge. No renderer implementation is substituted.
namespace facebook::react {
using Float = float;
using SurfaceId = int;
struct Point { Float x, y; };
struct Size { Float width, height; };
struct Rect { Point origin; Size size; };
struct Slot { uint32_t identifier; Rect rect; };
struct DocumentMeasurement { Size size; std::vector<Slot> slots, tagSlots; };
class DownstreamTextLayoutManager {
public:
  // PRODUCTION_DECLARATIONS
};
static const downstream::Theme::Handle &resolve(const downstream::Theme::Handle &theme) {
  return theme ? theme : downstream::Theme::defaultTheme();
}
// PRODUCTION_METHODS
}

static NSUInteger builds, layouts;
static IMP originalAlloc, originalLayout;
static id countedAlloc(id cls, SEL selector) {
  builds++;
  return reinterpret_cast<id (*)(id, SEL)>(originalAlloc)(cls, selector);
}
static void countedLayout(id object, SEL selector, NSTextContainer *container) {
  layouts++;
  reinterpret_cast<void (*)(id, SEL, NSTextContainer *)>(originalLayout)(object, selector, container);
}
static void require(BOOL condition, const char *message) {
  if (!condition) { fprintf(stderr, "FAIL: %s (builds=%lu layouts=%lu)\n", message,
                             (unsigned long)builds, (unsigned long)layouts); exit(1); }
}
static downstream::DocumentHandle document(bool code) {
  static uint64_t identifier = 981000;
  auto value = std::make_shared<downstream::Document>();
  value->id = identifier++;
  downstream::Block block;
  block.kind = code ? downstream::BlockKind::CodeBlock : downstream::BlockKind::Paragraph;
  block.text = u"const answer = 42;";
  block.language = "javascript";
  if (code) block.tokens.push_back({0, 5, downstream::TokenClass::Keyword});
  value->blocks.push_back(std::move(block));
  return value;
}
static DSRenderedDocument *mount(const downstream::DocumentHandle &doc, const downstream::LayoutHint &hint) {
  return [DSRenderedDocument renderedDocumentFor:doc fontScale:hint.fontScale syntaxHighlighting:hint.syntaxHighlighting
                                         theme:hint.theme caret:hint.caret];
}
static CGSize size(DSRenderedDocument *rendered, const downstream::LayoutHint &hint) {
  return [rendered sizeForWidth:hint.width chrome:hint.chrome code:hint.codeLayout() images:hint.images.state
                         hosts:downstream::HostLayout{} tables:downstream::tableLayout(hint.tables, hint.choices)];
}
static void check(bool highlighted) {
  [DSRenderedDocument invalidateCaches];
  builds = layouts = 0;
  auto doc = document(true);
  downstream::LayoutHint hint;
  hint.width = 320;
  hint.code.wrap = true;
  hint.code.lineNumbers = false;
  hint.syntaxHighlighting = highlighted;
  facebook::react::DownstreamTextLayoutManager manager;
  manager.prepare(doc, {hint});
  const NSUInteger preparedBuilds = builds, preparedLayouts = layouts;
  const auto measured = manager.measurePlatform(doc, hint.fontScale, hint.width, 0, hint.chrome, hint.code,
      hint.choices, hint.caret, hint.images, {}, hint.theme, hint.tables, false, highlighted);
  DSRenderedDocument *mounted = mount(doc, hint);
  const CGSize mountedSize = size(mounted, hint);
  require(builds == 1 && builds == preparedBuilds, "prepare/measure/mount built distinct renderings");
  // A cold code rendering shapes its intrinsic width and its wrapped width.
  require(layouts == 2 && layouts == preparedLayouts, "prepare/measure/mount repeated native layout");
  require(fabs(measured.size.height - mountedSize.height) < .001, "measurement/mount geometry differs");
  DSCodeSegment *segment = mounted.segments.firstObject;
  NSAttributedString *original = segment.text.attributedString;
  hint.syntaxHighlighting = !highlighted;
  manager.prepare(doc, {hint});
  DSRenderedDocument *switched = mount(doc, hint);
  DSCodeSegment *switchedSegment = switched.segments.firstObject;
  require(switched != mounted && builds == 2, "highlight toggle reused incorrect rendering");
  UIColor *before = [original attribute:NSForegroundColorAttributeName atIndex:0 effectiveRange:NULL];
  UIColor *after = [switchedSegment.text.attributedString attribute:NSForegroundColorAttributeName atIndex:0 effectiveRange:NULL];
  require(![before isEqual:after], "highlight toggle failed to change token color");
  require(CGSizeEqualToSize(mountedSize, size(switched, hint)), "highlight toggle changed geometry");
  require([segment.text.attributedString isEqualToAttributedString:original], "toggle mutated old rendering");
  hint.syntaxHighlighting = highlighted;
  manager.prepare(doc, {hint});
  require(mount(doc, hint) == mounted && builds == 2, "toggle back lost cached rendering");
  printf("ios_preparation highlighted=%d initial_builds=%lu initial_layouts=%lu toggle_colors=1 geometry_parity=1\n",
         highlighted, (unsigned long)preparedBuilds, (unsigned long)preparedLayouts);
}
int main() {
  @autoreleasepool {
    // Install an override on this metaclass rather than changing NSObject's
    // inherited allocation implementation for every class in the process.
    Class meta = object_getClass(DSRenderedDocument.class);
    originalAlloc = class_getMethodImplementation(meta, @selector(alloc));
    class_addMethod(meta, @selector(alloc), (IMP)countedAlloc, "@@:");
    Method layout = class_getInstanceMethod(NSLayoutManager.class, @selector(ensureLayoutForTextContainer:));
    originalLayout = method_setImplementation(layout, (IMP)countedLayout);
    check(false);
    check(true);
    [DSRenderedDocument invalidateCaches];
    builds = layouts = 0;
    auto prose = document(false);
    downstream::LayoutHint hint;
    hint.width = 320;
    hint.syntaxHighlighting = false;
    facebook::react::DownstreamTextLayoutManager::prepare(prose, {hint});
    DSRenderedDocument *plain = mount(prose, hint);
    hint.syntaxHighlighting = true;
    facebook::react::DownstreamTextLayoutManager::prepare(prose, {hint});
    require(mount(prose, hint) == plain && builds == 1 && layouts == 1, "code-free flag normalization regressed");
    method_setImplementation(layout, originalLayout);
    puts("preparation policy checks passed");
  }
  return 0;
}
