import { DownstreamMarkdown as NativeMarkdown } from './DownstreamMarkdown';
import { useDownstreamDefaults } from './DownstreamProvider';
import {
  mergeDownstreamOptions,
  resolveDownstreamOptions,
  type DownstreamProps,
} from './downstreamOptions';
import { warnDeprecated } from './deprecations';
import { resolvePlugins } from './plugins';
import { mathAvailable, warnAboutUnusedMathEngine } from './parseFlags';

export type { DownstreamProps } from './downstreamOptions';

/** Render a complete Markdown string or a response that is still arriving. */
export function Downstream({
  children,
  markdown,
  stream,
  ...options
}: DownstreamProps) {
  const defaults = useDownstreamDefaults();
  const { featurePlugins, ...appOptions } = mergeDownstreamOptions(
    defaults,
    resolveDownstreamOptions(options)
  );
  const features = resolvePlugins(featurePlugins ?? []);
  const configured = mergeDownstreamOptions(features.options, appOptions);
  const resolved = {
    ...configured,
    math: features.native.math ? (configured.math ?? true) : false,
    syntaxHighlighting: features.native.code,
  };
  if (
    typeof __DEV__ === 'boolean' &&
    __DEV__ &&
    !features.native.math &&
    mathAvailable()
  )
    warnAboutUnusedMathEngine();
  const sources =
    Number(children !== undefined) +
    Number(markdown !== undefined) +
    Number(stream !== undefined);
  if (
    sources !== 1 ||
    (children !== undefined && typeof children !== 'string')
  ) {
    throw new Error(
      'Downstream requires exactly one string children, markdown, or stream source.'
    );
  }
  if (stream !== undefined)
    return <NativeMarkdown {...resolved} stream={stream} />;
  const source = children ?? markdown;
  if (typeof source !== 'string')
    throw new Error('Downstream requires a string source.');
  return <NativeMarkdown {...resolved} markdown={source} />;
}

/** @deprecated Use Downstream. */
export function DownstreamMarkdown(props: DownstreamProps) {
  warnDeprecated('DownstreamMarkdown', 'Downstream');
  return <Downstream {...props} />;
}
