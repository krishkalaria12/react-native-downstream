import type { APIRoute } from 'astro';
import { getCollection } from 'astro:content';
import { docGroups } from '../data/nav';
import { site } from '../data/site';

/** A plain-text map of the docs for language models, following llmstxt.org. */
export const GET: APIRoute = async ({ site: base }) => {
  const docs = new Map(
    (await getCollection('docs')).map((entry) => [entry.id, entry])
  );
  const url = (path: string) => new URL(path, base).href;

  const sections = docGroups.map((group) => {
    const lines = group.items.map((item) => {
      const entry = docs.get(item.slug);
      return `- [${item.title}](${url(`/docs/${item.slug}.md`)}): ${entry?.data.description ?? ''}`;
    });
    return `## ${group.label}\n\n${lines.join('\n')}`;
  });

  const body = `# ${site.name}

> ${site.description}

Install with \`npm install ${site.packageName}\`. It needs the React Native New Architecture and a native build; Expo Go cannot load it, an Expo development build can.

${sections.join('\n\n')}

## Optional

- [Benchmarks](${url('/benchmarks')}): six React Native Markdown libraries measured on a OnePlus Nord CE 3.
- [Source code](${site.repo})
- [All docs in one file](${url('/llms-full.txt')})
`;

  return new Response(body, {
    headers: { 'Content-Type': 'text/plain; charset=utf-8' },
  });
};
