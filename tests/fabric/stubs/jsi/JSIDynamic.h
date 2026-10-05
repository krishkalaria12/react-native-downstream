// A parse-only stand-in for jsi/JSIDynamic.h.
//
// React Native's `react/bridging/Dynamic.h` includes this file to specialize
// bridging for `folly::dynamic`, and the specialization's bodies call the two
// functions below. Both live in `namespace facebook::jsi`, which is why they are
// declared there and not in `facebook::react`. Nothing here is ever compiled
// into a shipped binary.

#pragma once

#include <folly/dynamic.h>
#include <jsi/jsi.h>

namespace facebook::jsi {

folly::dynamic dynamicFromValue(Runtime &rt, const Value &value);
Value valueFromDynamic(Runtime &rt, const folly::dynamic &value);

} // namespace facebook::jsi