# iOS code-layout benchmark

`tests/ios/run-native-harness.sh` is a simulator harness for the native TextKit
paths used by code cards. It compiles the production Objective-C++ translation
units with the selected Xcode SDK, then runs UIKit/TextKit on an iOS simulator.

```sh
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
  tests/ios/run-native-harness.sh
```

The script links the production rendering, layout, theme, and segment-plan
translation units. `DSFontsShim.mm` is the only shim: it maps the native theme
font request to system UIKit fonts because this standalone target does not load
React Native's font resolver. The output reports alternating wrapped/unbounded
layout time, counts actual TextKit layout calls, and bounds the character reads
performed by a real gutter draw over 4,096 blank lines. Checks also cover
concurrent cache hits, three-width eviction, segment ownership after the card and
document are released, emoji line numbering, and changes to expand state.

The simulator is selected from the installed iPhone devices, preferring a booted
one. Set `IOS_SIMULATOR_UDID` to choose a specific device. Temporary compilation
files are removed after the run.

To compare with PR #19, extract its source into a temporary directory and run
the same harness with `DOWNSTREAM_IOS_SOURCE_ROOT` pointing there. This selects
the old source and disables checks for APIs introduced by the optimization.

```sh
baseline=$(mktemp -d)
git archive e680e62a1b601a361339085cbcc5902c6104d558 src/ios src/cpp | tar -x -C "$baseline"
DOWNSTREAM_IOS_SOURCE_ROOT="$baseline" tests/ios/run-native-harness.sh
rm -rf "$baseline"
```

The fixtures construct native code documents directly. Parsing, actual React
Native font resolution, streaming scheduling, selection interaction, and
physical-device frame time are outside this harness.
