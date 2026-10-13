import { fileURLToPath } from 'node:url';
import react from '@vitejs/plugin-react';
import { defineConfig } from 'vitest/config';

export default defineConfig({
  plugins: [react()],
  resolve: {
    alias: {
      // The library imports a handful of runtime values from `react-native`.
      // A stub keeps unit tests off a device without a preset that would drag
      // the whole React Native runtime in.
      'react-native': fileURLToPath(
        new URL('./tests/react-native.ts', import.meta.url)
      ),
    },
  },
  test: {
    globalSetup: ['./tests/engine.ts'],
    environment: 'happy-dom',
    globals: true,
    // Native binaries are built once in global setup before test workers start.
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
      exclude: ['**/tests/**', '**/*.config.ts'],
      reporter: ['text', 'html'],
    },
  },
});
