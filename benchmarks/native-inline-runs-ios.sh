#!/bin/bash
set -euo pipefail
repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
export DOWNSTREAM_IOS_HARNESS_MAIN="$repo/tests/ios/Sources/InlineRunsHarness/main.mm"
export DOWNSTREAM_IOS_HARNESS_SUCCESS="native inline run checks passed"
exec bash "$repo/tests/ios/run-native-harness.sh"
