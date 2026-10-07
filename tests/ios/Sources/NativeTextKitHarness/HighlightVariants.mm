static void checkHighlightVariants() {
  [DSRenderedDocument invalidateCaches];
  const auto obtain = [](const downstream::DocumentHandle &document, BOOL highlighting) {
    return [DSRenderedDocument renderedDocumentFor:document fontScale:1 syntaxHighlighting:highlighting
                                             theme:downstream::Theme::defaultTheme()
                                             caret:downstream::PlacedCaret{}];
  };
  auto prose = std::make_shared<downstream::Document>(*makeDocument(@"ordinary prose"));
  prose->blocks.tail()[0].kind = downstream::BlockKind::Paragraph;
  DSRenderedDocument *prepared = obtain(prose, YES);
  const downstream::CodeLayout code;
  const downstream::ImageStateLayout images;
  const downstream::HostLayout hosts;
  const CGSize preparedSize = [prepared sizeForWidth:320 chrome:0 code:code images:images hosts:hosts];
  Method ensureMethod = class_getInstanceMethod([NSLayoutManager class], @selector(ensureLayoutForTextContainer:));
  originalEnsureLayout = method_setImplementation(ensureMethod, (IMP)countedEnsureLayout);
  layoutCalls = 0;
  DSRenderedDocument *mounted = obtain(prose, NO);
  const CGSize mountedSize = [mounted sizeForWidth:320 chrome:0 code:code images:images hosts:hosts];
  method_setImplementation(ensureMethod, originalEnsureLayout);
  require(prepared == mounted && CGSizeEqualToSize(preparedSize, mountedSize),
          @"code-free highlighting variants missed prepared rendering");
  require(layoutCalls == 0, @"code-free mounting repeated prepared TextKit layout");

  auto mixed = std::make_shared<downstream::Document>(*makeDocument(@"keyword"));
  mixed->blocks.tail()[0].tokens.push_back({0, 7, downstream::TokenClass::Keyword});
  DSRenderedDocument *colored = obtain(mixed, YES);
  DSRenderedDocument *plain = obtain(mixed, NO);
  require(colored != plain, @"code highlighting variants were merged");
  DSCodeSegment *coloredCode = colored.segments.firstObject;
  DSCodeSegment *plainCode = plain.segments.firstObject;
  require(![coloredCode.text.attributedString isEqualToAttributedString:plainCode.text.attributedString],
          @"code highlighting toggle lost its token colors");

  // A committed prose segment in a mixed document is still independent of
  // the paint choice for its neighboring code segment.
  auto mixedProse = std::make_shared<downstream::Document>();
  mixedProse->id = mixed->id + 10000000;
  downstream::Block prefix = prose->blocks[0];
  prefix.identity = 889999;
  mixedProse->blocks.push_back(std::move(prefix));
  mixedProse->blocks.push_back(mixed->blocks[0]);
  mixedProse->settledBlocks = 1;
  DSRenderedDocument *mixedColored = obtain(mixedProse, YES);
  DSRenderedDocument *mixedPlain = obtain(mixedProse, NO);
  require(mixedColored.segments.firstObject == mixedPlain.segments.firstObject,
          @"committed prose rebuilt when code highlighting changed");
  printf("highlight_variants code_free_layout_calls=%lu shared_prose=1 distinct_code=1\n",
         (unsigned long)layoutCalls.load());
  [DSRenderedDocument invalidateCaches];
}
