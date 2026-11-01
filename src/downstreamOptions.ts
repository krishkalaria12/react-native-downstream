import type { ComponentType } from 'react';
import { warnDeprecated } from './deprecations';
import type { DownstreamPlugin } from './plugins';
import type {
  CustomRendererProps,
  FallbackRendererProps,
  MarkdownLinksOptions,
  MarkdownPlugins,
  MarkdownStreamController,
  MarkdownViewProps,
  TagRendererProps,
} from './types';

/** Options after the public convenience props are resolved for native rendering. */
export type ResolvedDownstreamOptions = Omit<
  MarkdownViewProps,
  'markdown' | 'stream'
> &
  Readonly<{ featurePlugins?: readonly DownstreamPlugin[] }>;

export type DownstreamCodeBlockOptions = Readonly<{
  wrap?: MarkdownViewProps['codeBlockWrap'];
  lineNumbers?: MarkdownViewProps['lineNumbers'];
  maxHeight?: MarkdownViewProps['codeBlockMaxHeight'];
}>;

export type DownstreamTableOptions = Readonly<{
  maxHeight?: MarkdownViewProps['tableMaxHeight'];
  onFullscreen?: MarkdownViewProps['onTableFullscreen'];
  renderFullscreen?: MarkdownViewProps['renderTableFullscreen'];
}>;

export type DownstreamImagesOptions = Readonly<{
  allowedPrefixes?: MarkdownViewProps['allowedImagePrefixes'];
  onPress?: MarkdownViewProps['onImagePress'];
}>;

export type DownstreamLinksOptions = MarkdownLinksOptions &
  Readonly<{
    safety?: MarkdownViewProps['linkSafety'];
    onPress?: MarkdownViewProps['onLinkPress'];
    onLongPress?: MarkdownViewProps['onLinkLongPress'];
    preview?: MarkdownViewProps['linkPreview'];
    disableAutolinkProtocols?: MarkdownViewProps['disableAutolinkProtocols'];
  }>;

export type DownstreamComponents = Readonly<{
  fences?: Readonly<Record<string, ComponentType<CustomRendererProps>>>;
  tags?: Readonly<Record<string, ComponentType<TagRendererProps>>>;
  fallback?: ComponentType<FallbackRendererProps>;
}>;

type LegacyAliases = Readonly<{
  /** @deprecated Use codeBlock.wrap. */
  codeBlockWrap?: MarkdownViewProps['codeBlockWrap'];
  /** @deprecated Use codeBlock.lineNumbers. */
  lineNumbers?: MarkdownViewProps['lineNumbers'];
  /** @deprecated Use codeBlock.maxHeight. */
  codeBlockMaxHeight?: MarkdownViewProps['codeBlockMaxHeight'];
  /** @deprecated Use table.maxHeight. */
  tableMaxHeight?: MarkdownViewProps['tableMaxHeight'];
  /** @deprecated Use table.onFullscreen. */
  onTableFullscreen?: MarkdownViewProps['onTableFullscreen'];
  /** @deprecated Use table.renderFullscreen. */
  renderTableFullscreen?: MarkdownViewProps['renderTableFullscreen'];
  /** @deprecated Use images.allowedPrefixes. */
  allowedImagePrefixes?: MarkdownViewProps['allowedImagePrefixes'];
  /** @deprecated Use images.onPress. */
  onImagePress?: MarkdownViewProps['onImagePress'];
  /** @deprecated Use links.safety. */
  linkSafety?: MarkdownViewProps['linkSafety'];
  /** @deprecated Use links.onPress. */
  onLinkPress?: MarkdownViewProps['onLinkPress'];
  /** @deprecated Use links.onLongPress. */
  onLinkLongPress?: MarkdownViewProps['onLinkLongPress'];
  /** @deprecated Use links.preview. */
  linkPreview?: MarkdownViewProps['linkPreview'];
  /** @deprecated Use links.disableAutolinkProtocols. */
  disableAutolinkProtocols?: MarkdownViewProps['disableAutolinkProtocols'];
}>;

export type DownstreamOptions = Omit<
  ResolvedDownstreamOptions,
  | keyof LegacyAliases
  | 'images'
  | 'links'
  | 'plugins'
  | 'featurePlugins'
  | 'math'
  | 'syntaxHighlighting'
> &
  LegacyAliases &
  Readonly<{
    codeBlock?: DownstreamCodeBlockOptions;
    table?: DownstreamTableOptions;
    images?: boolean | DownstreamImagesOptions;
    links?: DownstreamLinksOptions;
    components?: DownstreamComponents;
    /** Feature packages. Renderer objects remain a deprecated compatibility form. */
    plugins?: readonly DownstreamPlugin[] | MarkdownPlugins;
  }>;

export type DownstreamProps = DownstreamOptions &
  (
    | Readonly<{ children: string; markdown?: never; stream?: never }>
    | Readonly<{ markdown: string; children?: never; stream?: never }>
    | Readonly<{
        stream: MarkdownStreamController;
        children?: never;
        markdown?: never;
      }>
  );

const aliases = {
  codeBlockWrap: 'codeBlock.wrap',
  lineNumbers: 'codeBlock.lineNumbers',
  codeBlockMaxHeight: 'codeBlock.maxHeight',
  tableMaxHeight: 'table.maxHeight',
  onTableFullscreen: 'table.onFullscreen',
  renderTableFullscreen: 'table.renderFullscreen',
  allowedImagePrefixes: 'images.allowedPrefixes',
  onImagePress: 'images.onPress',
  linkSafety: 'links.safety',
  onLinkPress: 'links.onPress',
  onLinkLongPress: 'links.onLongPress',
  linkPreview: 'links.preview',
  disableAutolinkProtocols: 'links.disableAutolinkProtocols',
} as const;

/** Strip omitted values so an explicit undefined inherits the provider default. */
function defined<T extends object>(value: T): Partial<T> {
  return Object.fromEntries(
    Object.entries(value).filter(([, entry]) => entry !== undefined)
  ) as Partial<T>;
}

export function resolveDownstreamOptions(
  options: DownstreamOptions
): ResolvedDownstreamOptions {
  const { codeBlock, table, images, links, components, plugins, ...rest } =
    options;
  for (const removed of ['math', 'syntaxHighlighting'] as const) {
    if (removed in options)
      throw new Error(
        `Downstream: ${removed} was replaced by an explicit feature plugin. Use plugins={[${removed === 'math' ? 'math' : 'code'}()]}.`
      );
  }
  const featurePlugins = isFeaturePlugins(plugins) ? plugins : undefined;
  const legacyPlugins = isFeaturePlugins(plugins) ? undefined : plugins;
  for (const key of Object.keys(aliases) as Array<keyof typeof aliases>) {
    if (options[key] !== undefined) warnDeprecated(key, aliases[key]);
  }
  if (legacyPlugins?.renderers !== undefined)
    warnDeprecated('plugins.renderers', 'components.fences');
  if (legacyPlugins?.tags !== undefined)
    warnDeprecated('plugins.tags', 'components.tags');
  if (legacyPlugins?.fallbackComponent !== undefined)
    warnDeprecated('plugins.fallbackComponent', 'components.fallback');

  const imageOptions = typeof images === 'object' ? images : undefined;
  const hasComponents = components !== undefined || legacyPlugins !== undefined;
  return defined({
    ...rest,
    featurePlugins,
    codeBlockWrap: codeBlock?.wrap ?? rest.codeBlockWrap,
    lineNumbers: codeBlock?.lineNumbers ?? rest.lineNumbers,
    codeBlockMaxHeight: codeBlock?.maxHeight ?? rest.codeBlockMaxHeight,
    tableMaxHeight: table?.maxHeight ?? rest.tableMaxHeight,
    onTableFullscreen: table?.onFullscreen ?? rest.onTableFullscreen,
    renderTableFullscreen:
      table?.renderFullscreen ?? rest.renderTableFullscreen,
    images:
      images === undefined
        ? undefined
        : typeof images === 'boolean'
          ? images
          : true,
    allowedImagePrefixes:
      imageOptions?.allowedPrefixes ?? rest.allowedImagePrefixes,
    onImagePress: imageOptions?.onPress ?? rest.onImagePress,
    links:
      links === undefined
        ? undefined
        : defined({
            relative: links.relative,
            defaultOrigin: links.defaultOrigin,
          }),
    linkSafety: links?.safety ?? rest.linkSafety,
    onLinkPress: links?.onPress ?? rest.onLinkPress,
    onLinkLongPress: links?.onLongPress ?? rest.onLinkLongPress,
    linkPreview: links?.preview ?? rest.linkPreview,
    disableAutolinkProtocols:
      links?.disableAutolinkProtocols ?? rest.disableAutolinkProtocols,
    plugins: hasComponents
      ? {
          renderers: [
            ...Object.entries(components?.fences ?? {}).map(
              ([language, component]) => ({ language, component })
            ),
            ...(legacyPlugins?.renderers ?? []),
          ],
          tags: [
            ...Object.entries(components?.tags ?? {}).map(
              ([name, component]) => ({ name, component })
            ),
            ...(legacyPlugins?.tags ?? []),
          ],
          fallbackComponent:
            components?.fallback ?? legacyPlugins?.fallbackComponent,
        }
      : undefined,
  });
}

function isFeaturePlugins(
  plugins: DownstreamOptions['plugins']
): plugins is readonly DownstreamPlugin[] {
  return Array.isArray(plugins);
}

/** Merge each normalized layer so local legacy aliases also beat inherited groups. */
export function mergeDownstreamOptions(
  defaults: ResolvedDownstreamOptions,
  own: ResolvedDownstreamOptions
): ResolvedDownstreamOptions {
  const merged = { ...defaults, ...defined(own) };
  if (own.theme !== undefined && defaults.theme !== undefined)
    merged.theme = [defaults.theme, own.theme].flat();
  if (own.markdownStyle !== undefined && defaults.markdownStyle !== undefined)
    merged.markdownStyle = [defaults.markdownStyle, own.markdownStyle].flat();
  if (own.links !== undefined && defaults.links !== undefined)
    merged.links = { ...defaults.links, ...own.links };
  if (own.syntax !== undefined && defaults.syntax !== undefined)
    merged.syntax = { ...defaults.syntax, ...own.syntax };
  if (own.plugins !== undefined && defaults.plugins !== undefined)
    merged.plugins = {
      renderers: [
        ...(own.plugins.renderers ?? []),
        ...(defaults.plugins.renderers ?? []),
      ],
      tags: [...(own.plugins.tags ?? []), ...(defaults.plugins.tags ?? [])],
      fallbackComponent:
        own.plugins.fallbackComponent ?? defaults.plugins.fallbackComponent,
    };
  return merged;
}
