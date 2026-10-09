#include "SnapshotVector.h"
#include "tests/check.h"

#include <algorithm>
#include <atomic>
#include <numeric>
#include <thread>
#include <vector>

using namespace downstream;

int main() {
  SnapshotVector<int> values;
  std::vector<int> expected;
  std::vector<SnapshotVector<int>> retained;
  uint64_t payloadBytes = 0;
  uint64_t indexBytes = 0;
  for (int i = 0; i < 6000; i++) {
    retained.push_back(values);
    const auto work = values.replaceSuffix(values.size(), &i, 1);
    payloadBytes += work.payloadBytes;
    indexBytes += work.indexBytes;
    expected.push_back(i);
    CHECK(values.size() == expected.size() && values.back() == i, "paged append has the exact length and last value");
    CHECK(values.flattenedBytes() == 0, "indexed publication reads do not flatten");
  }
  CHECK(payloadBytes < 2 * 1024 * 1024 && indexBytes < 20 * 1024 * 1024,
        "tiny publications copy bounded pages and logarithmic index paths");
  CHECK(std::equal(values.begin(), values.end(), expected.begin()), "non-contiguous iteration reads every page");
  CHECK(values.flattenedBytes() == 0, "range iteration does not request contiguous storage");
  CHECK(values.begin()[2050] == 2050 && values.end() - values.begin() == 6000,
        "random access iterator supports standard indexed reads");
  for (size_t i = 1; i < retained.size(); i += 97) {
    CHECK(retained[i].size() == i && retained[i].back() == static_cast<int>(i - 1),
          "retained roots preserve exact original length and values");
  }
  const auto old = values;
  const int replacement[] = {-1, -2, -3};
  values.replaceSuffix(2050, replacement, 3);
  CHECK(values.size() == 2053 && values[2049] == 2049 && values[2050] == -1 && values.back() == -3,
        "suffix replacement preserves stable entries across index branches");
  CHECK(old.size() == 6000 && old[2050] == 2050 && old.back() == 5999, "replacing a suffix never mutates an old root");
  values.replaceSuffix(64, nullptr, 0);
  CHECK(values.size() == 64 && values.back() == 63, "page-aligned truncation removes an entire suffix");
  values.replaceSuffix(0, nullptr, 0);
  CHECK(values.empty() && values.flat().empty(), "complete truncation leaves an empty immutable publication");
  values.replaceSuffix(0, replacement, 3);
  CHECK(values.flat() == std::vector<int>({-1, -2, -3}), "append after truncation ignores discarded pages");
  const auto same = values;
  values.replaceSuffix(values.size(), nullptr, 0);
  CHECK(values.sharesStorage(same), "an unchanged publication retains its exact root");

  std::atomic<bool> matches{true};
  std::vector<std::thread> readers;
  const auto snapshot = old;
  for (size_t thread = 0; thread < 8; thread++) {
    readers.emplace_back([&] {
      for (size_t pass = 0; pass < 32; pass++) {
        if (snapshot.flat() != expected || snapshot.data() != snapshot.flat().data() || snapshot[4097] != 4097) {
          matches.store(false);
        }
      }
    });
  }
  values.push_back(7);
  for (auto &reader : readers) reader.join();
  CHECK(matches.load(), "concurrent flat and indexed reads share a safe exact immutable publication");
  CHECK(snapshot.flattenedBytes() == expected.size() * sizeof(int), "a paged publication materializes once");

  SnapshotVector<int> flat;
  flat = expected;
  const auto oldFlat = flat;
  flat.replaceSuffix(2050, replacement, 3);
  CHECK(flat.size() == 2053 && flat.back() == -3 && oldFlat.flat() == expected,
        "first paged replacement safely converts an owned contiguous initial publication");
  SnapshotVector<int> rewritten;
  const auto largeWork = rewritten.replaceSuffix(0, expected.data(), expected.size());
  const auto retainedRewrite = rewritten;
  CHECK(largeWork.payloadBytes == expected.size() * sizeof(int) && largeWork.indexBytes > 0,
        "full replacement copies the token payload once");
  CHECK(rewritten.flattenedBytes() == 0 && std::equal(rewritten.begin(), rewritten.end(), expected.begin()),
        "full replacement remains immutable and directly readable without another flatten");
  const auto shortWork = rewritten.replaceSuffix(0, replacement, 3);
  CHECK(shortWork.payloadBytes == 3 * sizeof(int) && shortWork.indexBytes == largeWork.indexBytes,
        "a publication with no retained prefix has constant metadata work");
  CHECK(retainedRewrite.flat() == expected && rewritten.flat() == std::vector<int>({-1, -2, -3}),
        "full rewrites preserve all retained snapshots");
  SnapshotVector<int> aliased;
  aliased = expected;
  aliased.replaceSuffix(0, aliased.data() + 100, 200);
  CHECK(aliased.size() == 200 && aliased[0] == 100 && aliased.back() == 299,
        "a full rewrite copies aliased source storage before releasing its owner");
  const auto retainedShort = rewritten;
  const auto clearWork = rewritten.replaceSuffix(0, nullptr, 0);
  CHECK(rewritten.empty() && clearWork.payloadBytes == 0 && clearWork.indexBytes == 0 && retainedShort.back() == -3,
        "an empty full replacement clears without allocating or changing old snapshots");
  return ds_check_result();
}
