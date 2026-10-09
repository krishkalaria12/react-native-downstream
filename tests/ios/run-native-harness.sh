#!/bin/bash
set -euo pipefail

repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
source_root=${DOWNSTREAM_IOS_SOURCE_ROOT:-$repo}
ios_dir="$repo/tests/ios"
export DEVELOPER_DIR=${DEVELOPER_DIR:-/Applications/Xcode.app/Contents/Developer}
sdk=$(xcrun --sdk iphonesimulator --show-sdk-path)
clang=$(xcrun --find clang++)
target="$(uname -m)-apple-ios15.1-simulator"
device=${IOS_SIMULATOR_UDID:-}
if [ -z "$device" ]; then
  device=$(xcrun simctl list devices available -j | python3 -c '
import json, sys
phones = [d for group in json.load(sys.stdin)["devices"].values() for d in group if "iPhone" in d["name"]]
if not phones:
    sys.exit("No available iPhone simulator. Set IOS_SIMULATOR_UDID or install an iOS runtime.")
print(next((d for d in phones if d["state"] == "Booted"), phones[0])["udid"])
')
fi
build=$(mktemp -d "${TMPDIR:-/tmp}/downstream-ios-layout.XXXXXX")
trap 'rm -rf "$build"' EXIT

flags=(-target "$target" -std=c++20 -O2 -DNDEBUG -fobjc-arc -isysroot "$sdk")
for directory in ios cpp cpp/downstream cpp/theme cpp/highlight cpp/session; do
  flags+=("-I$source_root/src/$directory")
done
[ "$source_root" = "$repo" ] || flags+=(-DBASELINE)
objects=()
sources=(
  ios/DSRenderedDocument.mm ios/DSSegmentViews.mm ios/DSSegmentViewport.mm ios/DSTextSegmentView.mm \
  ios/DSCodeSegmentView.mm ios/DSTableSegmentView.mm \
  ios/DSLayoutManager.mm ios/DSThemeColors.mm \
  cpp/theme/Theme.cpp cpp/theme/ThemeLayout.cpp cpp/downstream/CodeLayout.cpp cpp/downstream/SegmentPlan.cpp
)
if [ -f "$source_root/src/cpp/downstream/TableLayout.cpp" ]; then
  sources+=(cpp/downstream/TableLayout.cpp)
fi
harness_sources=(DSFontsShim.mm)
harness_main=${DOWNSTREAM_IOS_HARNESS_MAIN:-$ios_dir/Sources/NativeTextKitHarness/main.mm}
if [ -f "$source_root/src/ios/DSBoxSegmentViews.mm" ]; then
  # Use the real box views and math renderer. Only React's image transport and
  # presentation entry point are unavailable in this standalone executable.
  sources+=(
    ios/DSBoxSegmentViews.mm ios/DSMathFonts.mm ios/DSFileExporter.mm
    cpp/downstream/ImageLayout.cpp cpp/downstream/ImageLoaderRegistry.cpp cpp/downstream/ImageSizeCache.cpp
    cpp/downstream/Export.cpp cpp/downstream/ExportNames.cpp cpp/downstream/Unicode.cpp cpp/downstream/WorkerPool.cpp
    cpp/math/MathEngine.cpp cpp/math/MathDisplayList.cpp cpp/math/MathLayout.cpp cpp/math/MathSpeech.cpp
  )
  flags+=("-I$ios_dir/Sources/NativeTextKitHarness/UnavailableReact")
  harness_sources+=(UnavailableReactServices.mm)
fi
for relative in "${sources[@]}"; do
  object="$build/$(basename "$relative").o"
  "$clang" "${flags[@]}" -c "$source_root/src/$relative" -o "$object"
  objects+=("$object")
done
for source in "${harness_sources[@]}"; do
  object="$build/$source.o"
  "$clang" "${flags[@]}" -c "$ios_dir/Sources/NativeTextKitHarness/$source" -o "$object"
  objects+=("$object")
done
"$clang" "${flags[@]}" -c "$harness_main" -o "$build/harness-main.o"
objects+=("$build/harness-main.o")
app="$build/NativeTextKitHarness.app"
mkdir -p "$app"
"$clang" -target "$target" -isysroot "$sdk" "${objects[@]}" \
  -framework Foundation -framework UIKit -framework CoreGraphics -framework CoreText -framework QuartzCore \
  -o "$app/NativeTextKitHarness"
cp "$ios_dir/NativeTextKitHarness-Info.plist" "$app/Info.plist"
xcrun simctl boot "$device" >/dev/null 2>&1 || true
xcrun simctl bootstatus "$device" -b >/dev/null
xcrun simctl install "$device" "$app"
output=$(xcrun simctl launch --console-pty "$device" com.downstream.NativeTextKitHarness)
printf '%s\n' "$output"
[[ "$output" == *"${DOWNSTREAM_IOS_HARNESS_SUCCESS:-native TextKit checks passed}"* ]]
