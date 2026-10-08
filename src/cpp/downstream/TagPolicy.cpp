#include "TagPolicy.h"

#include <algorithm>

namespace downstream {

TagPolicy defaultTagPolicy() { return TagPolicy{}; }

TagPolicy tagPolicyFromStrings(const std::vector<std::string> &flat) {
  constexpr const char *kTagStart = "-1";
  TagPolicy policy;
  size_t i = 0;
  while (i < flat.size()) {
    if (flat[i] != kTagStart) {
      // Not our shape. Refused whole rather than read as names: a policy read
      // half right is a policy that allows an attribute nobody named.
      return TagPolicy{};
    }
    if (++i >= flat.size()) {
      return TagPolicy{};
    }
    AllowedTag tag;
    tag.name = flat[i++];
    while (i < flat.size() && flat[i] != kTagStart) {
      tag.attributes.push_back(flat[i++]);
    }
    policy.tags.push_back(std::move(tag));
  }
  // Sorted, because the policy compares by value and a lookup binary-searches.
  std::sort(policy.tags.begin(), policy.tags.end(),
            [](const AllowedTag &a, const AllowedTag &b) { return a.name < b.name; });
  return policy;
}

} // namespace downstream