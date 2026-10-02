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
build=$(mktemp -d "${TMPDIR:-/tmp}/downstream-ios-math.XXXXXX")
trap 'rm -rf "$build"' EXIT

flags=(-target "$target" -std=c++20 -O2 -DNDEBUG -fobjc-arc -isysroot "$sdk"
       "-I$source_root/src/ios" "-I$source_root/src/cpp")
[ "$source_root" = "$repo" ] || flags+=(-DBASELINE)
objects=()
for relative in ios/DSMathFonts.mm cpp/math/MathEngine.cpp cpp/math/MathDisplayList.cpp \
                cpp/math/MathLayout.cpp cpp/math/MathSpeech.cpp; do
  object="$build/$(basename "$relative").o"
  "$clang" "${flags[@]}" -c "$source_root/src/$relative" -o "$object"
  objects+=("$object")
done
"$clang" "${flags[@]}" -c "$ios_dir/Sources/MathAttachmentHarness/main.mm" -o "$build/main.o"
app="$build/MathAttachmentHarness.app"
mkdir -p "$app"
"$clang" -target "$target" -isysroot "$sdk" "${objects[@]}" "$build/main.o" \
  -framework Foundation -framework UIKit -framework CoreGraphics -framework CoreText \
  -o "$app/MathAttachmentHarness"
cp "$ios_dir/NativeTextKitHarness-Info.plist" "$app/Info.plist"
/usr/libexec/PlistBuddy -c 'Set :CFBundleExecutable MathAttachmentHarness' "$app/Info.plist"
/usr/libexec/PlistBuddy -c 'Set :CFBundleIdentifier com.downstream.MathAttachmentHarness' "$app/Info.plist"
/usr/libexec/PlistBuddy -c 'Set :CFBundleName MathAttachmentHarness' "$app/Info.plist"
xcrun simctl boot "$device" >/dev/null 2>&1 || true
xcrun simctl bootstatus "$device" -b >/dev/null
xcrun simctl install "$device" "$app"
output=$(xcrun simctl launch --console-pty "$device" com.downstream.MathAttachmentHarness)
printf '%s\n' "$output"
[[ "$output" == *"native math attachment checks passed"* ]]
