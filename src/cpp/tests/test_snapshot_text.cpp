#include "SnapshotText.h"
#include "SnapshotVector.h"
#include "tests/check.h"

#include <atomic>
#include <thread>
#include <vector>

using namespace downstream;

int main() {
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
