#!/bin/bash
set -euo pipefail
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
source_root=${DOWNSTREAM_IOS_SOURCE_ROOT:-$repo}
build=$(mktemp -d "${TMPDIR:-/tmp}/downstream-ios-preparation.XXXXXX")
trap 'rm -rf "$build"' EXIT
# Compile the exact production method bodies against the real renderer. Only
# React's geometry types and manager declaration are substituted in the fixture.
python3 - "$source_root" "$repo" "$build/main.mm" <<'PY'
import pathlib, sys
source, repo, output = map(pathlib.Path, sys.argv[1:])
manager = (source / 'src/ios/DownstreamTextLayoutManager.mm').read_text()
methods = manager[manager.index('DocumentMeasurement DownstreamTextLayoutManager::measurePlatform('):]
methods = methods[:methods.rindex('} // namespace facebook::react')]
measure = methods[:methods.index('{')].replace('DownstreamTextLayoutManager::', '')
prepare = methods[methods.index('void DownstreamTextLayoutManager::prepare('):]
prepare = prepare[:prepare.index('{')].replace('DownstreamTextLayoutManager::', '')
fixture = (repo / 'tests/ios/Sources/PreparationPolicyHarness/main.mm').read_text()
fixture = fixture.replace('// PRODUCTION_DECLARATIONS', measure + ';\nstatic ' + prepare + ';')
fixture = fixture.replace('// PRODUCTION_METHODS', methods)
output.write_text(fixture)
PY
DOWNSTREAM_IOS_HARNESS_MAIN="$build/main.mm" \
DOWNSTREAM_IOS_HARNESS_SUCCESS='preparation policy checks passed' \
  "$repo/tests/ios/run-native-harness.sh"
