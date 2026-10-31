import GithubSlugger from 'github-slugger';

const languageNames = {
  ts: 'TypeScript',
  tsx: 'TSX',
  js: 'JavaScript',
  jsx: 'JSX',
  json: 'JSON',
  sh: 'Terminal',
  bash: 'Terminal',
  shell: 'Terminal',
  swift: 'Swift',
  kotlin: 'Kotlin',
  ruby: 'Ruby',
  md: 'Markdown',
  markdown: 'Markdown',
  diff: 'Diff',
  text: 'Text',
  plaintext: 'Text',
};

const element = (tagName, properties, children = []) => ({ type: 'element', tagName, properties, children });
const text = (value) => ({ type: 'text', value });

/**
 * Shiki transformer that wraps every highlighted block in a frame with a
 * language label and a copy button. Works for Markdown and for <Code />.
 */
export const codeFrame = {
  name: 'downstream:code-frame',
  root(root) {
    const lang = this.options.lang ?? 'text';
    const title = this.options.meta?.__raw?.match(/title="([^"]+)"/)?.[1];
    const label = title ?? languageNames[lang] ?? lang;
    const head = element('div', { className: ['code-head'] }, [
      element('span', { className: ['code-lang'] }, [text(label)]),
      element('button', { type: 'button', className: ['code-copy'], 'data-copy-code': '' }, [text('Copy')]),
    ]);
    return {
      type: 'root',
      children: [element('div', { className: ['code-frame'], 'data-language': lang }, [head, ...root.children])],
    };
  },
};

/**
 * Sätteri hast plugin for docs: scrollable table wrappers and hoverable
 * heading anchors. Ids are assigned here so the anchor can point at them;
 * Astro's own heading-id pass keeps any id that already exists.
 */
export const docsHast = () => {
  const slugger = new GithubSlugger();
  return {
    name: 'downstream:docs',
    element: [
      {
        filter: ['table'],
        visit(node, ctx) {
          ctx.wrapNode(node, element('div', { className: ['table-wrap'] }));
        },
      },
      {
        filter: ['h2', 'h3'],
        visit(node, ctx) {
          const existing = node.properties?.id;
          const id = typeof existing === 'string' ? existing : slugger.slug(ctx.textContent(node));
          if (typeof existing !== 'string') ctx.setProperty(node, 'id', id);
          ctx.appendChild(
            node,
            element('a', { href: `#${id}`, className: ['heading-anchor'], ariaLabel: 'Link to this section' }, [text('#')]),
          );
        },
      },
    ],
  };
};
