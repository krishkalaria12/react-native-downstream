import type { APIRoute, GetStaticPaths } from 'astro';
import { getCollection, type CollectionEntry } from 'astro:content';

/** The raw Markdown of each doc page, linked from llms.txt. */
export const getStaticPaths = (async () =>
  (await getCollection('docs')).map((entry) => ({ params: { slug: entry.id }, props: { entry } }))) satisfies GetStaticPaths;

export const GET: APIRoute<{ entry: CollectionEntry<'docs'> }> = ({ props: { entry } }) =>
  new Response(`# ${entry.data.title}\n\n> ${entry.data.description}\n\n${entry.body?.trim() ?? ''}\n`, {
    headers: { 'Content-Type': 'text/markdown; charset=utf-8' },
  });
