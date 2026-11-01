import { DownstreamMarkdown as NativeMarkdown } from './DownstreamMarkdown';
import { useDownstreamDefaults } from './DownstreamProvider';
import {
  mergeDownstreamOptions,
  resolveDownstreamOptions,
  type DownstreamProps,
} from './downstreamOptions';
import { warnDeprecated } from './deprecations';

export type { DownstreamProps } from './downstreamOptions';

/** Render a complete Markdown string or a response that is still arriving. */
export function Downstream({
  children,
  markdown,
  stream,
  ...options
}: DownstreamProps) {
  const defaults = useDownstreamDefaults();
  const resolved = mergeDownstreamOptions(
    defaults,
    resolveDownstreamOptions(options)
  );
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
