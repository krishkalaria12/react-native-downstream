#include "RenderReuse.h"
#include "check.h"

#include <functional>
#include <iostream>

using namespace downstream;

int main() {
  Document live;
  live.id = 1;
  live.streaming = true;
  Block block;
  block.text = u"plain formula and link";
  block.runs.push_back({0, 5, 0, 0, -1});
  MathSpan formula;
  formula.tex = "x";
  formula.layout = std::make_shared<MathLayout>();
  block.math.push_back(formula);
  live.blocks.tail().push_back(block);
  Link link;
  link.url = "https://before.test";
  live.links.tail().push_back(link);
  Image image;
  image.url = "https://image.test/a.png";
  live.images.tail().push_back(image);
  Tag tag;
  tag.name = "span";
  live.tags.tail().push_back(tag);
  Document final = live;
  final.id = 2;
  final.streaming = false;
  final.settledBlocks = 1;
  CHECK(canReuseRendering(live, final), "unchanged identity-zero tail can share final render payload");
  CHECK(final.blocks[0].identity == 0, "render proof does not assign a provisional block identity");
  const auto rejects = [&](const std::function<void(Document &)> &change) {
    Document next = final;
    change(next);
    CHECK(!canReuseRendering(live, next), "changed rendering inputs invalidate final reuse");
  };
  rejects([](Document &d) { d.links.tail()[0].url = "https://after.test"; });
  rejects([](Document &d) { d.images.tail()[0].url = "https://image.test/b.png"; });
  rejects([](Document &d) { d.tags.tail()[0].attributes.emplace_back("style", "color:red"); });
  rejects([](Document &d) { d.blocks.tail()[0].text = u"different"; });
  rejects([](Document &d) { d.blocks.tail()[0].math[0].layout = std::make_shared<MathLayout>(); });
  rejects([](Document &d) { d.blocks.tail()[0].math[0].error = "final error"; });
  rejects([](Document &d) { d.blocks.tail()[0].highlightComplete = true; });
  rejects([](Document &d) { d.blocks.tail()[0].fenceOpen = true; });
  rejects([](Document &d) { d.blocks.tail()[0].fenceClosed = true; });
  rejects([](Document &d) { d.blocks.tail()[0].info = "ts file=a.ts"; });
  rejects([](Document &d) { d.blocks.tail()[0].direction = BlockDirection::RightToLeft; });
  rejects([](Document &d) { d.blocks.tail()[0].containers.push_back(Container{}); });
  Document differentTarget = final;
  differentTarget.links.tail()[0].url = "https://different.test";
  CHECK(scan::sameBlock(live.blocks[0], differentTarget.blocks[0]), "local reference indexes alone appear unchanged");
  CHECK(!sameRenderBlock(live, live.blocks[0], differentTarget, differentTarget.blocks[0]),
        "changed resolved link cannot preserve a native cache identity");
  Document differentAnswer = final;
  differentAnswer.blocks.tail()[0].math[0].error = "changed final answer";
  CHECK(!sameRenderBlock(live, live.blocks[0], differentAnswer, differentAnswer.blocks[0]),
        "changed math answer cannot preserve a native cache identity");

  Document uncommitted;
  for (size_t index = 0; index < kCommittedChunkBlocks + 4; ++index) {
    Block paragraph;
    paragraph.text = u"same";
    uncommitted.blocks.tail().push_back(paragraph);
  }
  Document regrouped = uncommitted;
  for (size_t index = 0; index < regrouped.blocks.size(); ++index) {
    regrouped.blocks.tail()[index].identity = index + 1;
  }
  CHECK(scan::sameDocument(uncommitted, regrouped), "payload comparison alone cannot prove segment boundaries");
  CHECK(!canReuseRendering(uncommitted, regrouped), "committed chunk regrouping requires a new layout payload");
  std::cout << "finish rendering equivalence guards passed\n";
  return ds_check_result();
}
