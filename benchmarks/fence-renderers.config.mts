import { createRequire } from 'node:module';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import react from '@vitejs/plugin-react';
import { defineConfig } from 'vitest/config';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const candidate = process.env.DOWNSTREAM_BENCH_SOURCE_ROOT ?? root;
const require = createRequire(import.meta.url);

export default defineConfig({
  plugins: [
    react(),
    {
      name: 'fence-benchmark-candidate',
      enforce: 'pre',
      resolveId(source, importer) {
        if (!importer?.endsWith('/benchmarks/fence-renderers.workload.tsx'))
          return null;
        if (source === '../src/fenceRenderers')
          return resolve(candidate, 'src/fenceRenderers.ts');
        if (source === '../src/NativeDownstreamSession')
          return resolve(candidate, 'src/NativeDownstreamSession.ts');
        if (source === '../src/DownstreamFenceSlotNativeComponent')
          return resolve(
            candidate,
            'src/DownstreamFenceSlotNativeComponent.ts'
          );
        return null;
      },
    },
  ],
  resolve: {
    // Both source roots share the exact installed React implementation.
    alias: [
      {
        find: /^(?:.*\/)?NativeDownstreamSession(?:\.ts)?$/,
        replacement: resolve(candidate, 'src/NativeDownstreamSession.ts'),
      },
      {
        find: /^(?:.*\/)?DownstreamFenceSlotNativeComponent(?:\.ts)?$/,
        replacement: resolve(
          candidate,
          'src/DownstreamFenceSlotNativeComponent.ts'
        ),
      },
      { find: /^react$/, replacement: require.resolve('react') },
      {
        find: /^react\/jsx-runtime$/,
        replacement: require.resolve('react/jsx-runtime'),
      },
      {
        find: /^react-native$/,
        replacement: resolve(root, 'tests/react-native.ts'),
      },
    ],
  },
  test: {
    environment: 'happy-dom',
    include: ['benchmarks/fence-renderers.workload.tsx'],
    testTimeout: 120_000,
    fileParallelism: false,
  },
});
