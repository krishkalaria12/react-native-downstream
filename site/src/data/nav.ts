export type DocLink = {
  slug: string;
  title: string;
};

export const docGroups: { label: string; items: DocLink[] }[] = [
  {
    label: 'Start here',
    items: [{ slug: 'getting-started', title: 'Getting started' }],
  },
  {
    label: 'Guide',
    items: [
      { slug: 'streaming', title: 'Streaming' },
      { slug: 'plugins', title: 'Plugins & defaults' },
      { slug: 'code-blocks', title: 'Code blocks' },
      { slug: 'tables', title: 'Tables' },
      { slug: 'images-and-math', title: 'Images & math' },
      { slug: 'custom-renderers', title: 'Custom renderers' },
      { slug: 'interaction', title: 'Links & interaction' },
      { slug: 'theming', title: 'Theming' },
    ],
  },
  {
    label: 'Reference',
    items: [
      { slug: 'api', title: 'API reference' },
      { slug: 'performance', title: 'Performance' },
      { slug: 'troubleshooting', title: 'Troubleshooting' },
    ],
  },
];

export const docLinks = docGroups.flatMap((group) => group.items);

export function neighbours(slug: string) {
  const index = docLinks.findIndex((link) => link.slug === slug);
  return {
    previous: index > 0 ? docLinks[index - 1] : null,
    next:
      index >= 0 && index < docLinks.length - 1 ? docLinks[index + 1] : null,
  };
}
