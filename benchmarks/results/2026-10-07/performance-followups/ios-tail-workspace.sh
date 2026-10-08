#!/bin/bash
set -euo pipefail
artifact_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo=$(CDPATH= cd -- "$artifact_dir/../../../.." && pwd)
baseline=$(mktemp -d "${TMPDIR:-/tmp}/downstream-ios-fresh.XXXXXX")
trap 'rm -rf "$baseline"' EXIT
mkdir -p "$baseline/src"
cp -R "$repo/src/ios" "$baseline/src/ios"
ln -s "$repo/src/cpp" "$baseline/src/cpp"
python3 - "$baseline/src/ios/DSRenderedDocument.mm" <<'PY'
import sys
path = sys.argv[1]
with open(path) as source:
    text = source.read()
condition = 'if ([segment allowsTailMeasurementReuse] && text.length <= 64 * 1024'
assert text.count(condition) == 1
text = text.replace(condition, 'if (false && [segment allowsTailMeasurementReuse] && text.length <= 64 * 1024')
with open(path, 'w') as source:
    source.write(text)
PY
DOWNSTREAM_IOS_SOURCE_ROOT="$baseline" bash "$repo/benchmarks/results/2026-10-07/performance-audit/ios-tail.sh" > "$artifact_dir/ios-tail-fresh.txt"
bash "$repo/benchmarks/results/2026-10-07/performance-audit/ios-tail.sh" > "$artifact_dir/ios-tail-workspace.txt"
