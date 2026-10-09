#import "DSSnapshotString.h"

static downstream::SnapshotText pagedSnapshot(const std::u16string &source) {
  downstream::SnapshotText snapshot;
  for (size_t offset = 0; offset < source.size(); offset += 137) {
    snapshot.appendSnapshot(std::u16string_view(source).substr(offset, 137));
  }
  return snapshot;
}

static NSData *snapshotPixels(NSAttributedString *text, CGFloat width) {
  UIGraphicsImageRendererFormat *format = [UIGraphicsImageRendererFormat new];
  format.scale = 1;
  format.opaque = YES;
  UIGraphicsImageRenderer *renderer = [[UIGraphicsImageRenderer alloc] initWithSize:CGSizeMake(width, 600)
                                                                           format:format];
  UIImage *image = [renderer imageWithActions:^(UIGraphicsImageRendererContext *context) {
    [UIColor.whiteColor setFill];
    CGContextFillRect(context.CGContext, CGRectMake(0, 0, width, 600));
    [text drawWithRect:CGRectMake(0, 0, width, 600)
              options:NSStringDrawingUsesLineFragmentOrigin | NSStringDrawingUsesFontLeading context:nil];
  }];
  return UIImagePNGRepresentation(image);
}

static void checkSnapshotStrings() {
  std::u16string source(1023, u'x');
  source += u"😀\nمرحبا é 日本語\nsecond line\n";
  auto snapshot = pagedSnapshot(source);
  const auto heldSnapshot = snapshot;
  NSString *string = [[DSSnapshotString alloc] initWithSnapshot:snapshot];
  NSString *flat = [NSString stringWithCharacters:reinterpret_cast<const unichar *>(source.data()) length:source.size()];
  require([string isEqualToString:flat] && string.length == source.size(), @"paged NSString lost UTF-16 units");
  require([string copy] == string, @"copy of immutable paged NSString made a new source buffer");
  require([string characterAtIndex:1023] == source[1023] && [string characterAtIndex:1024] == source[1024],
          @"paged NSString split surrogate characters incorrectly");
  std::vector<unichar> units(source.size());
  [string getCharacters:units.data() range:NSMakeRange(0, string.length)];
  require(std::equal(units.begin(), units.end(), source.begin()), @"whole range read changed UTF-16");
  NSString *suffix = [string substringFromIndex:1023];
  require([suffix isEqualToString:[flat substringFromIndex:1023]], @"snapshot suffix differed from flat NSString");
  require([[suffix substringWithRange:NSMakeRange(0, 2)] isEqualToString:@"😀"], @"nested snapshot slice lost emoji");
  require([[string substringWithRange:NSMakeRange(string.length, 0)] isEqualToString:@""], @"end slice was not empty");
  unichar emptyBuffer;
  [string getCharacters:&emptyBuffer range:NSMakeRange(string.length, 0)];
  for (NSRange range : {NSMakeRange(string.length + 1, 0), NSMakeRange(1, NSUIntegerMax)}) {
    BOOL raised = NO;
    @try { [string getCharacters:units.data() range:range]; }
    @catch (NSException *exception) { raised = [exception.name isEqualToString:NSRangeException]; }
    require(raised, @"paged NSString did not raise for invalid character range");
    raised = NO;
    @try { (void)[string substringWithRange:range]; }
    @catch (NSException *exception) { raised = [exception.name isEqualToString:NSRangeException]; }
    require(raised, @"paged NSString did not raise for invalid substring range");
  }
  BOOL raised = NO;
  @try { (void)[string characterAtIndex:string.length]; }
  @catch (NSException *exception) { raised = [exception.name isEqualToString:NSRangeException]; }
  require(raised, @"paged NSString did not raise for invalid index");
  snapshot.appendSnapshot(u"new tail");
  require([string isEqualToString:flat] && [suffix isEqualToString:[flat substringFromIndex:1023]],
          @"appending source changed an old immutable NSString");
  downstream::SnapshotText builder(u"builder");
  NSString *builderString = [[DSSnapshotString alloc] initWithSnapshot:builder];
  builder.append(u" changed");
  require([builderString isEqualToString:@"builder"], @"builder mutation changed published NSString");
  downstream::SnapshotText empty;
  require([[[DSSnapshotString alloc] initWithSnapshot:empty] isEqualToString:@""], @"empty snapshot string failed");
  NSMutableString *mutableString = [string mutableCopy];
  [mutableString appendString:@"!"];
  require(mutableString.length == flat.length + 1 && [string isEqualToString:flat], @"mutable copy changed source");
  std::atomic<bool> valid{true};
  auto *result = &valid;
  dispatch_apply(64, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^(size_t index) {
    @autoreleasepool {
      NSRange range = NSMakeRange(index * 13, 64);
      if (![[string substringWithRange:range] isEqualToString:[flat substringWithRange:range]]) result->store(false);
      unichar local[3];
      [string getCharacters:local range:NSMakeRange(1022, 3)];
      if (local[1] != 0xd83d || local[2] != 0xde00) result->store(false);
    }
  });
  require(valid.load(), @"concurrent snapshot range reads changed characters");
  require([string dataUsingEncoding:NSUTF8StringEncoding].length == [flat dataUsingEncoding:NSUTF8StringEncoding].length &&
          [[string dataUsingEncoding:NSUTF8StringEncoding] isEqual:[flat dataUsingEncoding:NSUTF8StringEncoding]],
          @"snapshot text export changed UTF-8 bytes");
  NSDictionary *attributes = @{NSFontAttributeName: [UIFont systemFontOfSize:16], NSForegroundColorAttributeName: UIColor.blackColor};
  NSAttributedString *pagedAttributes = [[NSAttributedString alloc] initWithString:string attributes:attributes];
  NSAttributedString *flatAttributes = [[NSAttributedString alloc] initWithString:flat attributes:attributes];
  require([pagedAttributes isEqualToAttributedString:flatAttributes], @"snapshot attributed string changed attributes");
  for (CGFloat width : {173., 320.}) {
    UITextView *pagedView = [[UITextView alloc] initWithFrame:CGRectMake(0, 0, width, 600)];
    UITextView *flatView = [[UITextView alloc] initWithFrame:CGRectMake(0, 0, width, 600)];
    pagedView.attributedText = pagedAttributes;
    flatView.attributedText = flatAttributes;
    require(CGSizeEqualToSize([pagedView sizeThatFits:CGSizeMake(width, CGFLOAT_MAX)],
                              [flatView sizeThatFits:CGSizeMake(width, CGFLOAT_MAX)]), @"snapshot TextKit height changed");
    pagedView.selectedRange = flatView.selectedRange = NSMakeRange(1023, 2);
    require([pagedView.textStorage.string substringWithRange:pagedView.selectedRange].length == 2 &&
            [[pagedView.textStorage.string substringWithRange:pagedView.selectedRange] isEqualToString:@"😀"],
            @"snapshot selection lost a surrogate pair");
    require([snapshotPixels(pagedAttributes, width) isEqual:snapshotPixels(flatAttributes, width)],
            @"paged NSString changed rendered glyph pixels");
  }
  require(snapshot.flattenedBytes() == 0 && heldSnapshot.flattenedBytes() == 0,
          @"Foundation or TextKit flattened native source pages");
  // Use production rendering, including code line indexes and first-line text.
  [DSRenderedDocument invalidateCaches];
  auto document = std::make_shared<downstream::Document>(*makeDocument(flat));
  document->blocks.tail()[0].text = pagedSnapshot(source);
  DSRenderedDocument *rendered = [DSRenderedDocument renderedDocumentFor:document fontScale:1 syntaxHighlighting:NO
                                             theme:downstream::Theme::defaultTheme() caret:downstream::PlacedCaret{}];
  DSCodeSegment *code = rendered.segments.firstObject;
  require([code.text.attributedString.string isEqualToString:flat], @"production code snapshot changed text");
  require(code.lineCount == 4 && [code.firstLine isEqualToString:[flat substringToIndex:1025]],
          @"production code line metadata changed");
  (void)[code.text heightForWidth:173];
  require(document->blocks[0].text.flattenedBytes() == 0, @"production rendering flattened native source pages");
  auto proseDocument = std::make_shared<downstream::Document>(*makeDocument(flat));
  proseDocument->blocks.tail()[0].kind = downstream::BlockKind::Paragraph;
  proseDocument->blocks.tail()[0].text = pagedSnapshot(source);
  proseDocument->blocks.tail()[0].runs.push_back({1023, 1025, downstream::InlineStyle::Strong});
  DSTextSegment *prose = [DSRenderedDocument renderedDocumentFor:proseDocument fontScale:1 syntaxHighlighting:NO
                                             theme:downstream::Theme::defaultTheme() caret:downstream::PlacedCaret{}].segments.firstObject;
  auto flatProseDocument = std::make_shared<downstream::Document>(*makeDocument(flat));
  flatProseDocument->blocks.tail()[0].kind = downstream::BlockKind::Paragraph;
  flatProseDocument->blocks.tail()[0].runs.push_back({1023, 1025, downstream::InlineStyle::Strong});
  DSTextSegment *flatProse = [DSRenderedDocument renderedDocumentFor:flatProseDocument fontScale:1 syntaxHighlighting:NO
                                             theme:downstream::Theme::defaultTheme() caret:downstream::PlacedCaret{}].segments.firstObject;
  require([prose.attributedString.string isEqualToString:flatProse.attributedString.string] &&
          [prose heightForWidth:173] == [flatProse heightForWidth:173] &&
          [snapshotPixels(prose.attributedString, 173) isEqual:snapshotPixels(flatProse.attributedString, 173)],
          @"production styled prose snapshot changed layout or pixels");
  require(proseDocument->blocks[0].text.flattenedBytes() == 0, @"production prose flattened native source pages");
  const std::u16string rowSource = source + u"\tright 😀";
  auto table = std::make_shared<downstream::Document>(*makeDocument(@""));
  auto &row = table->blocks.tail()[0];
  row.kind = downstream::BlockKind::TableRow;
  row.table = 1;
  row.text = pagedSnapshot(rowSource);
  row.cells = {{0, static_cast<uint32_t>(source.size())},
               {static_cast<uint32_t>(source.size() + 1), static_cast<uint32_t>(rowSource.size())}};
  DSTableSegment *tableSegment = renderTable(table);
  require([tableSegment.rows[0][0].text.string isEqualToString:flat] &&
          [tableSegment.rows[0][1].text.string isEqualToString:@"right 😀"], @"production table snapshot changed cell ranges");
  require(table->blocks[0].text.flattenedBytes() == 0, @"production table flattened native source pages");
  auto emptyProseDocument = std::make_shared<downstream::Document>(*makeDocument(@""));
  emptyProseDocument->blocks.tail()[0].kind = downstream::BlockKind::Paragraph;
  DSTextSegment *emptyProse = [DSRenderedDocument renderedDocumentFor:emptyProseDocument fontScale:1 syntaxHighlighting:NO
                                             theme:downstream::Theme::defaultTheme() caret:downstream::PlacedCaret{}].segments.firstObject;
  require([emptyProse.attributedString.string isEqualToString:@"\u200b"], @"empty prose lost its layout placeholder");
  printf("ios_snapshot_string source_flattened_bytes=0 utf16=1 ranges=1 snapshots=1 concurrent=1 attributes=1 pixels=1 selection=1 export=1 production=1\n");
}
