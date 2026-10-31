import { defineConfig } from 'astro/config';
import { satteri } from '@astrojs/markdown-satteri';
import sitemap from '@astrojs/sitemap';
import tailwindcss from '@tailwindcss/vite';
import { codeFrame, docsHast } from './src/lib/markdown.mjs';

export default defineConfig({
  site: 'https://downstream.krishkalaria.dev',
  // Pages build to `docs/streaming.html`, so every URL is served, linked and
  // listed without a trailing slash and each page has one canonical address.
  trailingSlash: 'never',
  build: { format: 'file' },
  integrations: [sitemap({ filter: (page) => !page.endsWith('/404') })],
  prefetch: {
    prefetchAll: false,
    defaultStrategy: 'hover',
  },
  vite: {
    plugins: [tailwindcss()],
  },
  markdown: {
    processor: satteri({ hastPlugins: [docsHast] }),
    shikiConfig: {
      theme: 'github-dark-default',
      transformers: [codeFrame],
    },
  },
});
