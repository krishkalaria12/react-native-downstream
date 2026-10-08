#include "Controls.h"

static void checkDetailsVisibility() {
  auto document = std::make_shared<downstream::Document>();
  document->id = 920001;
  downstream::Container details;
  details.kind = downstream::ContainerKind::Details;
  details.id = 1;
  details.showsMarker = true;
  downstream::Block summary;
  summary.kind = downstream::BlockKind::Paragraph;
  summary.text = u"Regression summary";
  summary.containers = {details};
  document->blocks.push_back(summary);
  details.showsMarker = false;
  downstream::Block body;
  body.kind = downstream::BlockKind::Paragraph;
  body.text = u"Hidden body";
  body.containers = {details};
  document->blocks.push_back(body);
  downstream::Block code = body;
  code.kind = downstream::BlockKind::CodeBlock;
  code.text = u"hidden code";
  document->blocks.push_back(code);
  downstream::Block after;
  after.kind = downstream::BlockKind::Paragraph;
  after.text = u"AFTER_DETAILS";
  document->blocks.push_back(after);
  DSRenderedDocument *rendered = [DSRenderedDocument renderedDocumentFor:document fontScale:1
      syntaxHighlighting:NO theme:downstream::Theme::defaultTheme() caret:downstream::PlacedCaret{}];
  require(rendered.segments.count == 4, @"details visibility boundaries were merged");
  downstream::CodeLayout layout;
  const downstream::ImageStateLayout images;
  const downstream::HostLayout hosts;
  const downstream::TableLayout tables;
  NSArray<NSValue *> *closed = [rendered segmentFramesForWidth:320 chrome:0 code:layout images:images hosts:hosts tables:tables];
  require(closed[0].CGRectValue.size.height > 0, @"closed summary disappeared");
  require(closed[1].CGRectValue.size.height == 0 && closed[2].CGRectValue.size.height == 0,
          @"closed details body or code retained height");
  require(CGRectGetMinY(closed[3].CGRectValue) < 100, @"hidden details left a gap before trailing prose");
  DSDocumentView *view = [DSDocumentView new];
  view.frame = CGRectMake(0, 0, 320, 500);
  [view setRendered:rendered];
  view.controls = downstream::kControlAll;
  [view layoutIfNeeded];
  require(view.subviews.count == 4 && view.subviews[1].hidden && view.subviews[2].hidden && !view.subviews[3].hidden,
          @"collapsed native views remained visible");
  DSTextSegmentView *summaryView = (DSTextSegmentView *)view.subviews[0];
  NSArray *closedAccessibility = summaryView.accessibilityElements;
  require([((UIAccessibilityElement *)closedAccessibility.firstObject).accessibilityValue isEqualToString:@"collapsed"],
          @"closed summary accessibility state missing");
  downstream::ReaderChoices openChoices = layout.choices.with(downstream::ChoiceKind::DetailsOpen, 1, true);
  summaryView.detailsChoices = openChoices;
  require(((UIAccessibilityElement *)summaryView.accessibilityElements.firstObject).accessibilityValue == nil,
          @"summary accessibility state stayed collapsed after opening");
  summaryView.detailsChoices = layout.choices;
  UIGraphicsBeginImageContextWithOptions(CGSizeMake(320, 100), NO, 1);
  @try {
    [summaryView.layoutManager drawBackgroundForGlyphRange:NSMakeRange(0, summaryView.layoutManager.numberOfGlyphs)
                                                atPoint:CGPointZero];
  } @catch (NSException *exception) {
    require(NO, [@"empty details marker crashed drawing: " stringByAppendingString:exception.reason]);
  }
  UIGraphicsEndImageContext();
  layout.choices = layout.choices.with(downstream::ChoiceKind::DetailsOpen, 1, true);
  view.codeLayout = layout;
  [view layoutIfNeeded];
  require(!view.subviews[1].hidden && !view.subviews[2].hidden, @"opening details did not restore body views");
  NSArray<NSValue *> *opened = [rendered segmentFramesForWidth:320 chrome:0 code:layout images:images hosts:hosts tables:tables];
  require(CGRectGetMinY(opened[3].CGRectValue) > CGRectGetMinY(closed[3].CGRectValue),
          @"opening details did not move following prose");
  printf("ios_details summary_visible=1 hidden_body=0 hidden_code=0 trailing_prose=1 reopen=1 empty_marker_draw=1\n");
}
