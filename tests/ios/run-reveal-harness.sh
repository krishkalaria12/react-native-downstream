#!/bin/bash
set -euo pipefail
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
export DOWNSTREAM_IOS_HARNESS_MAIN="$here/Sources/RevealHarness/main.mm"
export DOWNSTREAM_IOS_HARNESS_SUCCESS="native reveal checks passed"
exec bash "$here/run-native-harness.sh"
