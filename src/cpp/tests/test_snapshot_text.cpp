#include "SnapshotText.h"
#include "SnapshotVector.h"
#include "tests/check.h"

#include <atomic>
#include <thread>
#include <vector>

using namespace downstream;

int main() {
  {
    SnapshotText builder(std::u16string(2049, u'a'));
    const auto view = builder.chunk(0);
    const auto owner = builder.chunkOwner(0);
    CHECK(builder.chunkCount() == 3 && view.size() == 1024 && builder.chunk(2).size() == 1,
          "builder chunks have the same bounded shape as persistent pages");
    builder.assign(u"changed");
    CHECK(view[0] == u'a' && owner != nullptr, "owned builder chunk survives later mutation");

    SnapshotText pages;
    std::u16string source(3073, u'x');
    source[1023] = 0xd83d;
    source[1024] = 0xde00;
    source[2048] = u'\n';
    pages.appendSnapshot(source);
    const auto firstOwner = pages.chunkOwner(0);
    std::weak_ptr<const void> lastOwner = pages.chunkOwner(3);
    for (size_t start : {size_t{0}, size_t{1023}, size_t{1024}, size_t{2048}, source.size()}) {
      for (size_t count : {size_t{0}, size_t{1}, size_t{1031}, std::u16string::npos}) {
        CHECK(pages.copyRange(start, count) == source.substr(start, count),
              "range copies match UTF-16 string semantics across pages and surrogate seams");
      }
    }
    std::u16string copied(source.size(), u'\0');
    pages.copyTo(copied.data(), 0, copied.size());
    CHECK(copied == source && pages.find(u'\n') == 2048 && pages.find(u'\n', 2049) == std::u16string::npos,
          "bridge copy and newline lookup preserve exact content");
    CHECK(pages.substr(1023, 2) == source.substr(1023, 2) && pages.flattenedBytes() == 0,
          "chunk, range, substring and newline operations never flatten the source");
    bool rejected = false;
    try { pages.copyTo(copied.data(), pages.size(), 1); } catch (const std::out_of_range &) { rejected = true; }
    CHECK(rejected, "invalid exact ranges fail instead of overflowing");
    rejected = false;
    try { pages.chunk(pages.chunkCount()); } catch (const std::out_of_range &) { rejected = true; }
    CHECK(rejected, "invalid chunk indexes fail");
    pages.appendSnapshot(u"more");
    CHECK(firstOwner == pages.chunkOwner(0) && lastOwner.expired(),
          "appends retain completed page identity and release an unretained replaced tail page");
  }
  SnapshotText builder;
  builder.append(u"builder");
  SnapshotText copiedBuilder = builder;
  builder.push_back(u'!');
  CHECK(copiedBuilder == u"builder", "builder writes detach from a copied snapshot");
  builder.assign(u"native reader", 6);
  builder += u" builder";
  CHECK(builder == u"native builder" && copiedBuilder == u"builder",
        "existing native assign and append operators retain builder semantics");

  SnapshotText text;
  std::u16string expected;
  std::vector<SnapshotText> retained;
  uint64_t storageBytes = 0;
  // Cross both page and index-level boundaries, including split surrogate
  // units and newline units. Indexing never requires a contiguous string.
  const std::u16string delta = u"0123456789\n\xd83d\xde00\u6f22xy";
  for (size_t i = 0; i < 5000; i++) {
    retained.push_back(text);
    storageBytes += text.appendSnapshot(delta);
    expected.append(delta);
    CHECK(text.size() == expected.size() && text.back() == expected.back(), "page appends preserve exact length and tail");
    CHECK(text.flattenedBytes() == 0, "indexed parser reads do not materialize contiguous text");
  }
  CHECK(storageBytes < 20 * 1024 * 1024, "tiny append storage is bounded rather than copying every growing prefix");
  for (size_t i = 0; i < retained.size(); i += 97) {
    CHECK(retained[i].size() == i * delta.size(), "retained roots keep their original lengths");
    if (!retained[i].empty()) CHECK(retained[i][retained[i].size() - 1] == u'y', "retained pages keep their original text");
  }
  std::atomic<bool> matches{true};
  std::vector<std::thread> readers;
  const SnapshotText snapshot = text;
  for (size_t i = 0; i < 8; i++) {
    readers.emplace_back([&] {
      for (size_t read = 0; read < 32; read++) {
        if (snapshot.flat() != expected || snapshot.data() != snapshot.flat().data()) matches.store(false);
      }
    });
  }
  text.appendSnapshot(u"later");
  for (auto &reader : readers) reader.join();
  CHECK(matches.load(), "concurrent readers materialize one immutable exact snapshot safely");
  CHECK(snapshot.flattenedBytes() == expected.size() * sizeof(char16_t), "repeated materialization copies the exact text once");
  CHECK(text.flattenedBytes() == 0, "append does not carry or mutate the previous flat cache");
  text.resize(7);
  text.append(u"builder again");
  CHECK(snapshot.flat() == expected, "builder mutation of paged text preserves earlier published cache");

  SnapshotVector<int> tokens;
  tokens = std::vector<int>{1, 2, 3};
  const SnapshotVector<int> oldTokens = tokens;
  CHECK(tokens.data() == oldTokens.data(), "snapshot token copies share immutable published storage");
  tokens.push_back(4);
  CHECK(oldTokens.size() == 3 && tokens.size() == 4 && oldTokens[2] == 3,
        "explicit token builder mutation preserves retained publication");
  return ds_check_result();
}
