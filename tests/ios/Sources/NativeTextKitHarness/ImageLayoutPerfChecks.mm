// Included by the native harness after its shared require helper.
#include "Controls.h"
#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

@interface DSImageSegmentView (ImagePerfPrivate)
@property(nonatomic, readonly) downstream::ImageStateLayout state;
@property(nonatomic, readonly) NSArray<UIView *> *boxes;
@end

static IMP imagePerfOriginalGetters[6];
static NSUInteger imagePerfGetterCounts[6];
static downstream::ImageStateLayout imagePerfStateGetter(id object, SEL selector) {
  imagePerfGetterCounts[0]++;
  return reinterpret_cast<downstream::ImageStateLayout (*)(id, SEL)>(imagePerfOriginalGetters[0])(object, selector);
}
static std::vector<uint32_t> imagePerfImagesGetter(id object, SEL selector) {
  imagePerfGetterCounts[1]++;
  return reinterpret_cast<std::vector<uint32_t> (*)(id, SEL)>(imagePerfOriginalGetters[1])(object, selector);
}
static std::vector<std::string> imagePerfUrlsGetter(id object, SEL selector) {
  imagePerfGetterCounts[2]++;
  return reinterpret_cast<std::vector<std::string> (*)(id, SEL)>(imagePerfOriginalGetters[2])(object, selector);
}
static std::vector<std::string> imagePerfAltGetter(id object, SEL selector) {
  imagePerfGetterCounts[3]++;
  return reinterpret_cast<std::vector<std::string> (*)(id, SEL)>(imagePerfOriginalGetters[3])(object, selector);
}
static std::vector<std::string> imagePerfTitlesGetter(id object, SEL selector) {
  imagePerfGetterCounts[4]++;
  return reinterpret_cast<std::vector<std::string> (*)(id, SEL)>(imagePerfOriginalGetters[4])(object, selector);
}
static std::vector<int32_t> imagePerfLinksGetter(id object, SEL selector) {
  imagePerfGetterCounts[5]++;
  return reinterpret_cast<std::vector<int32_t> (*)(id, SEL)>(imagePerfOriginalGetters[5])(object, selector);
}

static DSImageSegment *imagePerfSegment(NSUInteger count, NSInteger changedURL = -1,
                                        BOOL loadable = NO, BOOL reverse = NO) {
  if (count == 0) return [DSImageSegment new];
  auto document = std::make_shared<downstream::Document>();
  static uint64_t identifier = 997000;
  document->id = identifier++;
  downstream::Block block;
  block.kind = downstream::BlockKind::Image;
  for (NSUInteger index = 0; index < count; index++) {
    downstream::Image image;
    image.url = "https://example.invalid/image-" + std::to_string(index) + (index == changedURL ? "-changed" : "") + ".png";
    image.alt = u"a picture with descriptive alternative text";
    image.title = "image title " + std::to_string(index);
    image.loadable = loadable;
    image.link = index % 2 == 0 ? static_cast<int32_t>(index) : -1;
    document->images.push_back(std::move(image));
    block.images.push_back(static_cast<uint32_t>(reverse ? count - index - 1 : index));
  }
  document->blocks.push_back(std::move(block));
  DSImageSegment *segment = [DSRenderedDocument renderedDocumentFor:document fontScale:1 syntaxHighlighting:NO
      theme:downstream::Theme::defaultTheme() caret:downstream::PlacedCaret{}].segments.firstObject;
  require([segment isKindOfClass:DSImageSegment.class], @"image fixture did not produce an image segment");
  return segment;
}

static downstream::ImageStateLayout imagePerfStates(NSUInteger count) {
  downstream::ImageStateLayout states;
  for (NSUInteger index = 0; index < count; index++) {
    states.entries.push_back({static_cast<uint32_t>(index), downstream::ImageState::Disabled, 0, 0});
  }
  return states;
}

static void checkImageValueCopies() {
  constexpr NSUInteger count = 128;
  DSImageSegment *segment = imagePerfSegment(count);
  DSImageSegmentView *view = [[DSImageSegmentView alloc] initWithFrame:CGRectMake(0, 0, 320, 100)];
  auto states = imagePerfStates(4096);
  [view setImageState:states];
  Class classes[] = {DSImageSegmentView.class, DSImageSegment.class, DSImageSegment.class,
                     DSImageSegment.class, DSImageSegment.class, DSImageSegment.class};
  const char *names[] = {"state", "images", "urls", "altTexts", "titles", "links"};
  IMP replacements[] = {(IMP)imagePerfStateGetter, (IMP)imagePerfImagesGetter, (IMP)imagePerfUrlsGetter,
                         (IMP)imagePerfAltGetter, (IMP)imagePerfTitlesGetter, (IMP)imagePerfLinksGetter};
  for (NSUInteger index = 0; index < 6; index++) {
    imagePerfOriginalGetters[index] = method_setImplementation(
        class_getInstanceMethod(classes[index], sel_registerName(names[index])), replacements[index]);
    imagePerfGetterCounts[index] = 0;
  }
  [view setSegment:segment];
  const NSUInteger assignmentImages = imagePerfGetterCounts[1];
  NSUInteger assignmentStrings = 0;
  for (NSUInteger index = 2; index < 6; index++) assignmentStrings += imagePerfGetterCounts[index];
#ifdef IOS_ROUND2_BASELINE
  require(assignmentImages >= count && assignmentStrings >= count * 4, @"baseline image assignment did not reproduce vector copying");
#else
  require(assignmentImages == 1 && assignmentStrings == 4, @"image assignment repeatedly copied vector properties");
#endif
  std::fill(std::begin(imagePerfGetterCounts), std::end(imagePerfGetterCounts), 0);
  [view layoutSubviews];
  const NSUInteger layoutStates = imagePerfGetterCounts[0], layoutImages = imagePerfGetterCounts[1];
#ifdef IOS_ROUND2_BASELINE
  require(layoutStates == count && layoutImages >= count, @"baseline image layout did not reproduce state and vector copying");
#else
  require(layoutStates == 0 && layoutImages == 0, @"image layout copied state or image vectors per box");
#endif
  for (NSUInteger index = 0; index < 6; index++) {
    method_setImplementation(class_getInstanceMethod(classes[index], sel_registerName(names[index])), imagePerfOriginalGetters[index]);
  }
  printf("ios_image_value_copies images=%lu assignment_image_getters=%lu assignment_metadata_getters=%lu layout_state_getters=%lu layout_image_getters=%lu\n",
         (unsigned long)count, (unsigned long)assignmentImages, (unsigned long)assignmentStrings,
         (unsigned long)layoutStates, (unsigned long)layoutImages);
}

static void checkImageLayoutGeometry() {
  constexpr NSUInteger count = 9;
  DSImageSegment *segment = imagePerfSegment(count);
  DSImageSegmentView *view = [DSImageSegmentView new];
  [view setSegment:segment];
  view.controls = downstream::ControlBits::ImageDownload;
  view.controlsEnabled = YES;
  downstream::ImageStateLayout states;
  for (NSUInteger index = 0; index < count - 1; index++) {
    const auto state = static_cast<downstream::ImageState>(index % 6);
    states.entries.push_back({static_cast<uint32_t>(index), state,
        index >= 6 || state == downstream::ImageState::Loaded ? 640.0f : 0,
        index >= 6 || state == downstream::ImageState::Loaded ? 480.0f : 0});
  }
  NSUInteger cases = 0;
  for (BOOL enabled : {NO, YES}) {
    states.enabled = enabled;
    [view setImageState:states];
    for (CGFloat scale : {1.0, 2.0, 3.0}) {
      view.screenScale = scale;
      for (CGFloat width : {0.0, 96.0, 320.0, 600.0}) {
        view.bounds = CGRectMake(0, 0, width, 200);
        [view layoutSubviews];
        CGFloat y = 0;
        for (NSUInteger index = 0; index < count; index++) {
          const downstream::ImageBox expected = [segment boxAtIndex:index column:width answers:states];
          UIView *box = view.boxes[index];
          require(CGRectEqualToRect(box.frame, CGRectMake(segment.contentLeading, y, expected.width, expected.height)),
                  @"image view geometry differed from the shared measured box");
          const downstream::ImageEntry *entry = states.find(static_cast<uint32_t>(index));
          const auto state = entry != nullptr ? entry->state : (enabled ? downstream::ImageState::Pending : downstream::ImageState::Disabled);
          UIView *picture = [box valueForKey:@"picture"];
          UIView *placeholder = [box valueForKey:@"placeholder"];
          UIView *fallback = [box valueForKey:@"fallback"];
          UIView *download = [box valueForKey:@"download"];
          require(picture.hidden == (state != downstream::ImageState::Loaded) &&
                  placeholder.hidden == !(state == downstream::ImageState::Pending || state == downstream::ImageState::Loading) &&
                  fallback.hidden == !(state == downstream::ImageState::Failed || state == downstream::ImageState::Blocked || state == downstream::ImageState::Disabled) &&
                  download.hidden == (state != downstream::ImageState::Loaded),
                  @"image state, fallback or control visibility changed");
          require(box.accessibilityLabel.length > 0, @"image lost its accessibility label");
          y += expected.height + segment.metrics.marginTop;
          cases++;
        }
      }
    }
  }
  printf("ios_image_geometry_cases=%lu states=6 scales=3 widths=4 missing_entry=1\n", (unsigned long)cases);
}

static void checkImageBoxRetirement() {
  DSImageSegmentView *view = [DSImageSegmentView new];
  [view setImageState:imagePerfStates(8)];
  DSImageSegment *initial = imagePerfSegment(4, -1, YES);
  [view setSegment:initial];
  NSArray<UIView *> *first = [view.boxes copy];
  [view setSegment:imagePerfSegment(4, 1, YES)];
  NSArray<UIView *> *changed = [view.boxes copy];
  require(changed[0] == first[0] && changed[1] != first[1] && changed[2] == first[2] && changed[3] == first[3] && first[1].superview == nil,
          @"image URL change replaced retained siblings or kept an obsolete box");
  [view setSegment:imagePerfSegment(3, 1, YES)];
  require(view.boxes[0] == changed[0] && view.boxes[1] == changed[1] && view.boxes[2] == changed[2] && changed[3].superview == nil,
          @"image shrink did not retire only the removed tail");
  NSArray<UIView *> *beforeReverse = [view.boxes copy];
  [view setSegment:imagePerfSegment(3, 1, YES, YES)];
  require(view.boxes[1] == beforeReverse[1] && beforeReverse[0].superview == nil && beforeReverse[2].superview == nil,
          @"image reordering incorrectly reused boxes across different indices");
  NSArray<UIView *> *beforeEmpty = [view.boxes copy];
  [view setSegment:imagePerfSegment(0)];
  require(view.boxes.count == 0, @"empty image segment retained its old boxes");
  for (UIView *box in beforeEmpty) require(box.superview == nil, @"empty image segment left an obsolete subview");
}

static IMP imagePerfOriginalAddAnimation;
static NSUInteger imagePerfPulseAdds;
static void imagePerfAddAnimation(id layer, SEL selector, CAAnimation *animation, NSString *key) {
  if ([key isEqualToString:@"ds.placeholder"]) imagePerfPulseAdds++;
  reinterpret_cast<void (*)(id, SEL, CAAnimation *, NSString *)>(imagePerfOriginalAddAnimation)(layer, selector, animation, key);
}
static void checkImagePulseReuse() {
  DSImageSegmentView *view = [[DSImageSegmentView alloc] initWithFrame:CGRectMake(0, 0, 320, 100)];
  [view setSegment:imagePerfSegment(1)];
  downstream::ImageStateLayout states;
  states.entries.push_back({0, downstream::ImageState::Pending, 0, 0});
  [view setImageState:states];
  Method add = class_getInstanceMethod(CALayer.class, @selector(addAnimation:forKey:));
  imagePerfOriginalAddAnimation = method_setImplementation(add, (IMP)imagePerfAddAnimation);
  imagePerfPulseAdds = 0;
  for (NSUInteger iteration = 0; iteration < 8; iteration++) [view layoutSubviews];
  UIView *placeholder = [view.boxes[0] valueForKey:@"placeholder"];
  const NSUInteger repeatedAdds = imagePerfPulseAdds;
  if (!UIAccessibilityIsReduceMotionEnabled()) {
#ifdef IOS_ROUND2_BASELINE
    require(repeatedAdds == 8, @"baseline did not restart the image pulse on every layout");
#else
    require(repeatedAdds == 1, @"image layout restarted an existing placeholder pulse");
#endif
    states.entries[0].state = downstream::ImageState::Failed;
    [view setImageState:states];
    [view layoutSubviews];
    require([placeholder.layer animationForKey:@"ds.placeholder"] == nil, @"failed image retained its loading pulse");
    states.entries[0].state = downstream::ImageState::Loading;
    [view setImageState:states];
    [view layoutSubviews];
    require(imagePerfPulseAdds == repeatedAdds + 1, @"loading transition did not start a new pulse");
    [placeholder.layer removeAllAnimations];
    [view layoutSubviews];
    require(imagePerfPulseAdds == repeatedAdds + 2, @"removed placeholder animation did not resume");
  } else {
    require(repeatedAdds == 0, @"Reduce Motion added a placeholder pulse");
  }
  method_setImplementation(add, imagePerfOriginalAddAnimation);
  printf("ios_image_pulse_adds=%lu layouts=8 reduce_motion=%d\n", (unsigned long)repeatedAdds, UIAccessibilityIsReduceMotionEnabled());
}

static void benchmarkImageLayoutCopies() {
  for (NSUInteger count : {NSUInteger(128), NSUInteger(512)}) {
    DSImageSegment *segment = imagePerfSegment(count);
    DSImageSegmentView *view = [[DSImageSegmentView alloc] initWithFrame:CGRectMake(0, 0, 320, 100)];
    [view setImageState:imagePerfStates(4096)];
    [view setSegment:segment];
    for (NSUInteger iteration = 0; iteration < 4; iteration++) [view layoutSubviews];
    constexpr NSUInteger iterations = 20;
    const auto start = std::chrono::steady_clock::now();
    for (NSUInteger iteration = 0; iteration < iterations; iteration++) {
      @autoreleasepool { [view layoutSubviews]; }
    }
    const double layoutMS = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    for (NSUInteger iteration = 0; iteration < 2; iteration++) [view setSegment:segment];
    const auto assignmentStart = std::chrono::steady_clock::now();
    for (NSUInteger iteration = 0; iteration < iterations; iteration++) {
      @autoreleasepool { [view setSegment:segment]; }
    }
    const double assignmentMS = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - assignmentStart).count();
    printf("ios_image_layout_ms=%.3f assignment_ms=%.3f images=%lu state_entries=4096 iterations=%lu\n",
           layoutMS, assignmentMS, (unsigned long)count, (unsigned long)iterations);
  }
}

static void checkImageLayoutPerformance() {
  checkImageValueCopies();
  checkImageLayoutGeometry();
  checkImageBoxRetirement();
  checkImagePulseReuse();
  benchmarkImageLayoutCopies();
}
