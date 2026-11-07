export const site = {
  name: 'Downstream',
  packageName: '@react-native-downstream/core',
  title: 'Downstream: Native Markdown for React Native',
  description:
    'A native Markdown renderer for React Native, built for streaming AI chat. Finished blocks are laid out once, parsing runs in native code, and code, tables and math are built in.',
  repo: 'https://github.com/krishkalaria12/react-native-downstream',
  npm: 'https://www.npmjs.com/package/@react-native-downstream/core',
  author: { name: 'Krish Kalaria', url: 'https://github.com/krishkalaria12' },
  image: {
    path: '/og.png',
    width: 1200,
    height: 630,
    alt: 'Downstream: Native Markdown for React Native',
  },
};

export type Schema = Record<string, unknown>;

/** Absolute URL for a site path. */
export const absolute = (path: string, base: URL | undefined) =>
  new URL(path, base ?? 'https://downstream.krishkalaria.in').href;

export const breadcrumbs = (
  items: { name: string; path: string }[],
  base: URL | undefined
): Schema => ({
  '@type': 'BreadcrumbList',
  'itemListElement': items.map((item, index) => ({
    '@type': 'ListItem',
    'position': index + 1,
    'name': item.name,
    'item': absolute(item.path, base),
  })),
});
