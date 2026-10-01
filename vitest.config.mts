import { existsSync } from 'node:fs';
import { fileURLToPath, pathToFileURL } from 'node:url';
import type { Plugin } from 'vite';
import react from '@vitejs/plugin-react';
import { defineConfig } from 'vitest/config';

/**
 * Some dependencies in the comparison bench ship imports Node's ESM resolver
 * rejects but Metro accepts: extensionless relative paths, and at least one
 * file that ships as `.ts` inside its compiled output. Retry those so the bench
 * can load the package unmodified.
 */
function resolveLooseImports(): Plugin {
  return {
    name: 'downstream:resolve-loose-imports',
    enforce: 'pre',
    resolveId(source, importer) {
      if (!importer || !source.startsWith('.')) return null;
      const from = pathToFileURL(importer);
      if (existsSync(fileURLToPath(new URL(source, from)))) return null;
      for (const extension of ['.js', '.mjs', '.cjs', '.ts', '.tsx']) {
        const candidate = new URL(`${source}${extension}`, from);
        if (existsSync(fileURLToPath(candidate))) return fileURLToPath(candidate);
      }
      return null;
    },
  };
}

export default defineConfig({
  plugins: [react(), resolveLooseImports()],
  // Streamdown's tree is pre-bundled so Vite's resolver, not Node's, handles
  // its extensionless and `.ts`-in-`lib` imports. Node's ESM resolver rejects
  // both; Metro accepts both, so this matches how the app actually loads it.
  optimizeDeps: {
    include: [
      'react-native-streamdown',
      'react-native-enriched-markdown',
      'remend',
    ],
  },
  resolve: {
    alias: {
      // The library imports a handful of runtime values from `react-native`.
      // A stub keeps unit tests and benchmarks off a device without a preset
      // that would drag the whole React Native runtime in.
      'react-native': fileURLToPath(
        new URL('./tests/react-native.ts', import.meta.url)
      ),
      // Streamdown's dependency declares `main` as "./lib/module/index" with no
      // extension, and ships some of its output as .ts. Metro accepts both;
      // Node's ESM resolver does not, so point at the real entry here.
      // Node has no worklet runtime, and Streamdown creates one at import time.
      'react-native-worklets': fileURLToPath(
        new URL('./benchmarks/worklets-stub.ts', import.meta.url)
      ),
      'react-native-enriched-markdown': fileURLToPath(
        new URL('./node_modules/react-native-enriched-markdown/lib/module/index.js', import.meta.url)
      ),
    },
  },
  // Streamdown's tree ships imports Node's ESM resolver rejects: an extensionless
  // `main`, and files that are `.ts` inside `lib/`. Metro accepts both. Vitest
  // externalizes node_modules by default, which hands them to Node, so these two
  // deps are inlined and go through Vite's resolver and transform instead.
  test: {
    server: {
      deps: {
        inline: [/react-native-streamdown/, /react-native-enriched-markdown/, /remend/],
      },
    },
    environment: 'happy-dom',
    globals: true,
    // The first parse compiles the C++ engine, which takes about a second here
    // and longer on a loaded CI runner. Every other test is far under the
    // default, so only the compile needs the headroom.
    testTimeout: 30_000,
    hookTimeout: 60_000,
    include: [
      'tests/**/*.test.ts',
      'tests/**/*.test.tsx',
      'example/src/**/*.test.ts',
      'example/src/**/*.test.tsx',
    ],
    coverage: {
      provider: 'v8',
      include: ['src/**/*.ts', 'src/**/*.tsx'],
      exclude: ['**/tests/**', '**/benchmarks/**', '**/*.config.ts'],
      reporter: ['text', 'html'],
    },
    benchmark: {
      include: [
        'benchmarks/**/*.bench.ts',
        'benchmarks/**/*.bench.tsx',
      ],
    },
  },
});
