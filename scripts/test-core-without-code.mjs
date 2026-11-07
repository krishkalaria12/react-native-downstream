import { execFileSync } from 'node:child_process';
import { join } from 'node:path';
import { compile, CPP, ENGINE, MD4C_SOURCES } from '../src/cpp/tests/build.mjs';
const binary = compile(
  [
    join(CPP, 'tests', 'test_core_without_code.cpp'),
    join(CPP, 'session', 'IncrementalParser.cpp'),
    ...ENGINE,
    ...MD4C_SOURCES,
  ],
  'downstream-core-without-code',
  false
);
execFileSync(binary, { stdio: 'inherit' });
console.log('Core without @react-native-downstream/code passed.');
