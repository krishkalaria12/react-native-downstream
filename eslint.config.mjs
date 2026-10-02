import { fixupConfigRules } from '@eslint/compat';
import { FlatCompat } from '@eslint/eslintrc';
import js from '@eslint/js';
import prettier from 'eslint-plugin-prettier';
import { defineConfig } from 'eslint/config';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);
const compat = new FlatCompat({
  baseDirectory: __dirname,
  recommendedConfig: js.configs.recommended,
  allConfig: js.configs.all,
});

export default defineConfig([
  {
    extends: fixupConfigRules(compat.extends('@react-native', 'prettier')),
    plugins: { prettier },
    rules: {
      'react/react-in-jsx-scope': 'off',
      'prettier/prettier': 'error',
    },
  },
  {
    files: ['**/*.mjs'],
    languageOptions: {
      ecmaVersion: 'latest',
      sourceType: 'module',
    },
  },
  {
    ignores: [
      'node_modules/',
      'lib/',
      '**/build/**',
      'example/ios/**',
      'example/android/**',
      'example/dist/**',
      // The theme slot table is written by a script. Prettier's opinion of it
      // is not the generator's, so linting it would mean an autofix silently
      // editing generated output and the drift check failing on the result.
      'src/cpp/theme/ThemeSlots.h',
      'src/themeSlots.ts',
      'src/android/src/main/java/com/downstream/ThemeSlots.kt',
    ],
  },
]);
