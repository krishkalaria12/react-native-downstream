#include "Document.h"
#include "check.h"

#include <string>
#include <vector>

using namespace downstream;

int main() {
  using Prefix = SharedList<std::string>::Prefix;
  std::shared_ptr<const Prefix> prefix;
  std::vector<std::shared_ptr<const Prefix>> snapshots;
  std::vector<const std::string *> addresses;
  for (size_t index = 0; index < 8193; ++index) {
    prefix = Prefix::extend(prefix, {std::to_string(index)});
    addresses.push_back(&(*prefix)[index]);
    if (index == 0 || index == 62 || index == 63 || index == 64 ||
        index == 2047 || index == 2048 || index == 8191) {
      snapshots.push_back(prefix);
    }
  }
  CHECK(prefix->size() == 8193, "all small extensions are indexed");
  for (size_t index = 0; index < prefix->size(); ++index) {
    CHECK((*prefix)[index] == std::to_string(index), "radix index preserves order");
    CHECK(&(*prefix)[index] == addresses[index], "extension preserves item addresses");
  }
  prefix.reset();
  for (const auto &snapshot : snapshots) {
    for (size_t index = 0; index < snapshot->size(); ++index) {
      CHECK((*snapshot)[index] == std::to_string(index), "retained snapshot owns all earlier chunks");
      CHECK(&(*snapshot)[index] == addresses[index], "retained snapshot references are stable");
    }
  }
  // A chunk crosses both leaf and branch boundaries; tails remain owned by
  // their lists and are not confused with prefix pages.
  std::vector<std::string> chunk;
  for (size_t index = 0; index < 4099; ++index) chunk.push_back(std::to_string(index));
  auto first = Prefix::extend(nullptr, std::move(chunk));
  auto second = Prefix::extend(first, {"4099", "4100"});
  SharedList<std::string> list(second, {"tail"});
  CHECK(first->size() == 4099 && second->size() == 4101, "batch append leaves prior size intact");
  CHECK(list.size() == 4102 && list.back() == "tail", "owned tail follows shared pages");
  size_t visited = 0;
  for (const auto &value : list) {
    CHECK(value == (visited == 4101 ? "tail" : std::to_string(visited)), "iteration covers pages and tail");
    ++visited;
  }
  CHECK(visited == list.size(), "iteration reaches every item");
  auto empty = Prefix::extend(nullptr, {});
  CHECK(empty->size() == 0, "empty prefix can be extended");
  CHECK(Prefix::extend(empty, {"first"})->size() == 1, "empty extension starts first page");
  std::vector<size_t> values(65537);
  for (size_t index = 0; index < values.size(); ++index) values[index] = index;
  auto wide = SharedList<size_t>::Prefix::extend(nullptr, std::move(values));
  for (size_t index : {size_t{0}, size_t{63}, size_t{2047}, size_t{2048}, size_t{65535}, size_t{65536}}) {
    CHECK((*wide)[index] == index, "batch crosses a third radix level");
  }
  return ds_check_result();
}
