import type { APIRoute } from 'astro';
import { getCollection } from 'astro:content';
import { docLinks } from '../data/nav';
import { site } from '../data/site';

/** Every doc page as Markdown, in reading order. */
export const GET: APIRoute = async () => {
  const docs = new Map(
    (await getCollection('docs')).map((entry) => [entry.id, entry])
  );
  const pages = docLinks.flatMap((link) => {
    const entry = docs.get(link.slug);
    return entry
      ? [
          `# ${entry.data.title}\n\n> ${entry.data.description}\n\n${entry.body?.trim() ?? ''}`,
        ]
      : [];
  });
  return new Response(
    `# ${site.name}\n\n> ${site.description}\n\n${pages.join('\n\n---\n\n')}\n`,
    {
      headers: { 'Content-Type': 'text/plain; charset=utf-8' },
    }
  );
};
