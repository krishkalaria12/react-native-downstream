# Downstream site

The landing page, documentation, and phone benchmarks for `react-native-downstream`.

Built with Astro and Tailwind CSS. Fonts are self-hosted with Fontsource.

## Develop

```sh
pnpm install
pnpm dev
```

The dev server runs at `http://localhost:4321`.

## Build

```sh
pnpm build
pnpm preview
```

Static output goes to `dist/`.

## Content

- `src/pages/index.astro` — landing page.
- `src/pages/benchmarks.astro` — measured phone benchmark tables and charts.
- `src/pages/docs/` — documentation index and dynamic doc route.
- `src/content/docs/*.md` — documentation pages.
- `src/data/benchmarks.ts` — benchmark numbers, relative multipliers, and the feature matrix.
- `src/data/nav.ts` — docs sidebar groups and order.

Benchmark numbers come from `benchmarks/phone/REPORT.md` in the repository root. Update both files together when the report changes.

## Design

Design tokens live in `src/styles/global.css`: paper, ink, yellow, blue, coral, and pink, with hard offset shadows, rotated cards, sticky notes, and dashed rules. Reusable pieces are in `src/components/`.
