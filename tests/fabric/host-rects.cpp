// Exercises hosted-frame lookup against the real React Native geometry types.
#include "fabric/DownstreamTextLayoutManager.h"

#include <cstdio>

namespace react = facebook::react;

int main() {
  react::DocumentMeasurement measurement;
  if (measurement.slotFor(0) != nullptr) return 1;
  for (uint32_t index = 0; index < 1024; index++) {
    const react::Float height = static_cast<react::Float>(index);
    measurement.slots.push_back({index * 3 + 2, react::Rect{react::Point{0, height}, react::Size{100, height}}});
  }
  for (uint32_t index = 0; index < 1024; index++) {
    const react::Rect *rect = measurement.slotFor(index * 3 + 2);
    if (rect == nullptr || rect->size.height != static_cast<react::Float>(index)) return 2;
    if (measurement.slotFor(index * 3) != nullptr) return 3;
    if (measurement.slotFor(index * 3 + 1) != nullptr) return 4;
  }
  if (measurement.slotFor(4096) != nullptr) return 5;
  std::puts("host rectangle lookup checks passed");
}
