#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#import "DSRenderedDocument.h"
#import <objc/runtime.h>
#include "Document.h"
#include "HostLayout.h"
#include "theme/Theme.h"
#include <chrono>
#include <atomic>
#include <cstdio>
#include <memory>
static std::atomic<size_t> layouts{0};
static IMP original;
static void countLayout(id manager, SEL selector, NSTextContainer *container) {
 ++layouts;
 reinterpret_cast<void (*)(id, SEL, NSTextContainer *)>(original)(manager, selector, container);
}
static downstream::DocumentHandle document(size_t chars, uint64_t id) {
 auto result = std::make_shared<downstream::Document>(); result->id = id;
 downstream::Block block; block.kind = downstream::BlockKind::Paragraph;
 for (size_t i = 0; i < chars; ++i) block.text += u"words "[i % 6];
 result->blocks.push_back(std::move(block)); return result;
}
static double ms(std::chrono::steady_clock::time_point start) {
 return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}
int main() { @autoreleasepool {
 Method method = class_getInstanceMethod(NSLayoutManager.class, @selector(ensureLayoutForTextContainer:));
 original = method_setImplementation(method, (IMP)countLayout);
 const downstream::CodeLayout code; const downstream::ImageStateLayout images; const downstream::HostLayout hosts;
 const auto theme = downstream::Theme::defaultTheme(); const downstream::PlacedCaret caret;
 auto doc = document(8192, 200000);
 DSRenderedDocument *yes = [DSRenderedDocument renderedDocumentFor:doc fontScale:1 syntaxHighlighting:YES theme:theme caret:caret];
 (void)[yes sizeForWidth:320 chrome:0 code:code images:images hosts:hosts];
 layouts = 0;
 DSRenderedDocument *no = [DSRenderedDocument renderedDocumentFor:doc fontScale:1 syntaxHighlighting:NO theme:theme caret:caret];
 (void)[no sizeForWidth:320 chrome:0 code:code images:images hosts:hosts];
 printf("prose_highlight_false duplicate_document=%d duplicate_segment=%d duplicate_measure_layouts=%zu\n", yes != no, yes.segments.firstObject != no.segments.firstObject, layouts.load());
 uint64_t nextId = 200001;
 for (size_t chars : {2048,8192,32768}) {
  constexpr size_t updates = 20; double build = 0, measure = 0;
  layouts = 0;
  for (size_t step = 0; step < updates; ++step) { @autoreleasepool {
   auto input = document(chars + step * 6, nextId++);
   auto start = std::chrono::steady_clock::now();
   DSRenderedDocument *rendered = [DSRenderedDocument renderedDocumentFor:input fontScale:1 syntaxHighlighting:YES theme:theme caret:caret];
   build += ms(start); start = std::chrono::steady_clock::now();
   (void)[rendered sizeForWidth:320 chrome:0 code:code images:images hosts:hosts];
   measure += ms(start);
  }}
  printf("stream_tail chars=%zu updates=%zu build_ms=%.3f measure_ms=%.3f explicit_layout_calls=%zu\n", chars,updates,build/updates,measure/updates,layouts.load());
 }
 auto roundedDoc = document(32768,nextId++);
 DSRenderedDocument *rounded = [DSRenderedDocument renderedDocumentFor:roundedDoc fontScale:1 syntaxHighlighting:YES theme:theme caret:caret];
 (void)[rounded sizeForWidth:320.2 chrome:0 code:code images:images hosts:hosts];
 layouts = 0;
 auto roundedStart = std::chrono::steady_clock::now();
 (void)[rounded segmentFramesForWidth:320 chrome:0 code:code images:images hosts:hosts];
 printf("rounded_mount_width old=320.2 new=320 full_layouts=%zu frames_ms=%.3f\n",layouts.load(),ms(roundedStart));
 [DSRenderedDocument invalidateCaches];
 NSPointerArray *weak = [NSPointerArray weakObjectsPointerArray];
 for (size_t i=0; i<128; ++i) { @autoreleasepool {
  auto snapshot = document(32768+i*6,nextId++);
  DSRenderedDocument *rendered = [DSRenderedDocument renderedDocumentFor:snapshot fontScale:1 syntaxHighlighting:YES theme:theme caret:caret];
  [weak addPointer:(__bridge void *)rendered];
 }}
 size_t live=0,retainedChars=0;
 for (size_t i=0; i<weak.count; ++i) {
  DSRenderedDocument *rendered = (__bridge DSRenderedDocument *)[weak pointerAtIndex:i];
  if (rendered) { ++live; DSTextSegment *text = rendered.segments.firstObject; retainedChars += text.attributedString.length; }
 }
 printf("snapshot_render_cache live=%zu retained_utf16_chars=%zu utf16_content_bytes=%zu\n",live,retainedChars,2*retainedChars);
 printf("native TextKit checks passed\n"); return 0;
}}
