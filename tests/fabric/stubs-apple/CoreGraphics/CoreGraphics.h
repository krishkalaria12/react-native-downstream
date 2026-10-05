#pragma once

// The one CoreGraphics name React Native's iOS `Float.h` reads, for checking the
// iOS flavour of the headers where there is no Apple SDK. `CGFloat` is a double
// on every 64-bit Apple platform, which is every platform React Native targets.
typedef double CGFloat;
