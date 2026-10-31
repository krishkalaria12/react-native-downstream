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
pnpm build     # static output in dist/
pnpm preview   # build, then serve dist/ with Wrangler as Cloudflare would
```

## Deploy

The site is served by Cloudflare Workers as static assets, with no Worker code. `wrangler.jsonc` sets the routing:

- `html_handling: drop-trailing-slash` serves `/docs/streaming` and redirects `/docs/streaming/` and `/docs/streaming.html` to it. This matches the canonical URLs and the sitemap.
- `not_found_handling: 404-page` serves `dist/404.html` with a 404 status.
- `public/_headers` sets long-lived caching for the hashed files in `/_astro` and a few security headers.

Deploy from your machine:

```sh
pnpm wrangler login   # once
pnpm deploy
```

To deploy on every push with Workers Builds, connect the repository in the Cloudflare dashboard and use:

| Setting           | Value                                                                                     |
| ----------------- | ----------------------------------------------------------------------------------------- |
| Root directory    | `site`                                                                                    |
| Build command     | `pnpm install --frozen-lockfile --filter "react-native-downstream-site..." && pnpm build` |
| Deploy command    | `pnpm exec wrangler deploy`                                                               |
| Build watch paths | `site/*`                                                                                  |
| Build variable    | `SKIP_DEPENDENCY_INSTALL=true`                                                            |

Skipping the automatic install and filtering it to the site keeps the build from installing the React Native example app. Node is pinned by `site/.node-version`, and pnpm by `packageManager` in the root `package.json`.

The site is served at `downstream.krishkalaria.in` through the `routes` entry in `wrangler.jsonc`. The `krishkalaria.in` zone must be on the same Cloudflare account. If the domain changes, update `site` in `astro.config.mjs` as well.

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
