import type { ComponentType } from 'react';
import type { ResolvedDownstreamOptions } from './downstreamOptions';
import type {
  CustomRendererProps,
  MarkdownViewProps,
  TagRendererProps,
} from './types';

export type DownstreamPluginParseOptions = Pick<
  MarkdownViewProps,
  | 'math'
  | 'inlineHtml'
  | 'syntax'
  | 'cjk'
  | 'links'
  | 'disableAutolinkProtocols'
  | 'allowedTags'
  | 'literalTagContent'
>;

/** A feature's parsing policy, native capability, renderers, and default styles. */
export type DownstreamPlugin = Readonly<{
  name: string;
  parse?: DownstreamPluginParseOptions;
  fences?: Readonly<Record<string, ComponentType<CustomRendererProps>>>;
  tags?: Readonly<Record<string, ComponentType<TagRendererProps>>>;
  native?: Readonly<{ code?: boolean; math?: boolean }>;
  style?: Readonly<{
    theme?: MarkdownViewProps['theme'];
    markdownStyle?: MarkdownViewProps['markdownStyle'];
  }>;
}>;

export type ResolvedDownstreamPlugins = Readonly<{
  options: ResolvedDownstreamOptions;
  native: Readonly<{ code: boolean; math: boolean }>;
  names: readonly string[];
}>;

function defined<T extends object>(value: T): Partial<T> {
  return Object.fromEntries(
    Object.entries(value).filter(([, entry]) => entry !== undefined)
  ) as Partial<T>;
}

/** Later plugin entries override earlier defaults; app options are applied afterward. */
export function resolvePlugins(
  plugins: readonly DownstreamPlugin[]
): ResolvedDownstreamPlugins {
  let options: ResolvedDownstreamOptions = {};
  const native = { code: false, math: false };
  const names = new Set<string>();

  for (const plugin of plugins) {
    if (
      typeof plugin !== 'object' ||
      plugin === null ||
      typeof plugin.name !== 'string' ||
      plugin.name.trim() === ''
    ) {
      throw new TypeError('Downstream plugins require a nonempty name.');
    }
    const name = plugin.name.trim();
    if (names.has(name)) {
      if (typeof __DEV__ === 'boolean' && __DEV__)
        throw new Error(`Duplicate Downstream plugin name: ${name}.`);
      continue;
    }
    names.add(name);

    const parse = plugin.parse;
    if (parse) {
      const previous = options;
      options = { ...previous, ...defined(parse) };
      if (parse.syntax !== undefined)
        options = {
          ...options,
          syntax: { ...previous.syntax, ...defined(parse.syntax) },
        };
      if (parse.links !== undefined)
        options = {
          ...options,
          links: { ...previous.links, ...defined(parse.links) },
        };
      if (parse.allowedTags !== undefined)
        options = {
          ...options,
          allowedTags: { ...previous.allowedTags, ...parse.allowedTags },
        };
    }

    for (const capability of ['code', 'math'] as const) {
      const value = plugin.native?.[capability];
      if (value === undefined) continue;
      if (typeof value !== 'boolean')
        throw new TypeError(
          `Downstream plugin ${name} native.${capability} must be a boolean.`
        );
      native[capability] = value;
    }

    if (plugin.fences !== undefined || plugin.tags !== undefined) {
      options = {
        ...options,
        plugins: {
          ...options.plugins,
          renderers: [
            ...Object.entries(plugin.fences ?? {}).map(
              ([language, component]) => ({ language, component })
            ),
            ...(options.plugins?.renderers ?? []),
          ],
          tags: [
            ...Object.entries(plugin.tags ?? {}).map(
              ([tagName, component]) => ({
                name: tagName,
                component,
              })
            ),
            ...(options.plugins?.tags ?? []),
          ],
        },
      };
    }

    if (plugin.style?.theme !== undefined) {
      options = {
        ...options,
        theme:
          options.theme === undefined
            ? plugin.style.theme
            : [options.theme, plugin.style.theme].flat(),
      };
    }
    if (plugin.style?.markdownStyle !== undefined) {
      options = {
        ...options,
        markdownStyle:
          options.markdownStyle === undefined
            ? plugin.style.markdownStyle
            : [options.markdownStyle, plugin.style.markdownStyle].flat(),
      };
    }
  }

  return { options, native, names: [...names] };
}
