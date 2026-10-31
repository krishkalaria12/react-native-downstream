import { getCollection, render } from 'astro:content';
import { docLinks } from './nav';

export type SearchEntry = {
  /** Page or heading text shown in results. */
  title: string;
  /** Page the entry belongs to; empty for a page entry itself. */
  page: string;
  href: string;
  /** Extra text that matches but is not shown in full. */
  text: string;
};

let index: Promise<SearchEntry[]> | undefined;

/** Built once per build: every doc page plus its h2 and h3 headings. */
export function getSearchIndex() {
  index ??= (async () => {
    const docs = await getCollection('docs');
    const order = new Map(docLinks.map((link, i) => [link.slug, i]));
    docs.sort((a, b) => (order.get(a.id) ?? 99) - (order.get(b.id) ?? 99));

    const entries: SearchEntry[] = [];
    for (const doc of docs) {
      const href = `/docs/${doc.id}`;
      entries.push({
        title: doc.data.title,
        page: '',
        href,
        text: doc.data.description,
      });
      const { headings } = await render(doc);
      for (const heading of headings) {
        if (heading.depth !== 2 && heading.depth !== 3) continue;
        entries.push({
          title: cleanHeading(heading.text),
          page: doc.data.title,
          href: `${href}#${heading.slug}`,
          text: '',
        });
      }
    }
    entries.push({
      title: 'Benchmarks',
      page: '',
      href: '/benchmarks',
      text: 'Phone benchmark app CPU memory janky frames frame time settle iOS simulator methodology',
    });
    return entries;
  })();
  return index;
}

/** Heading text with the trailing anchor glyph the docs plugin appends. */
export const cleanHeading = (text: string) => text.replace(/#$/, '').trim();
