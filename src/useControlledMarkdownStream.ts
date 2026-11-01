import { useLayoutEffect, useRef, useState } from 'react';
import {
  createMarkdownStream,
  getStreamState,
  type StreamState,
} from './createMarkdownStream';
import { resolveAllowedTags } from './allowedTags';
import { mathAvailable, resolveParseFlags } from './parseFlags';
import type { MarkdownViewProps } from './types';

type ControlledProps = Extract<MarkdownViewProps, { markdown: string }>;

/** A string view owns its session; creation and disposal only happen on commit. */
export function useControlledMarkdownStream(
  props: ControlledProps,
  enabled: boolean,
  fallbackSource: string
) {
  const latest = useRef(props);
  latest.current = props;
  const owned = useRef<StreamState | null>(null);
  const accepted = useRef(fallbackSource);
  if (!enabled) accepted.current = fallbackSource;
  const [stream, setStream] = useState<StreamState | null>(null);
  const flags = resolveParseFlags(
    {
      math: props.math,
      inlineHtml: props.inlineHtml,
      syntax: props.syntax,
      cjk: props.cjk,
      links: props.links,
      autolinkProtocols: props.disableAutolinkProtocols,
      literalTagContent: props.literalTagContent,
    },
    mathAvailable()
  ).flags;
  const tags = JSON.stringify(resolveAllowedTags(props.allowedTags).tags);

  useLayoutEffect(() => {
    if (!enabled) return;
    const current = latest.current;
    const state = getStreamState(
      createMarkdownStream({
        ...current,
        preview: current.preview ?? true,
        onError: (error) => latest.current.onError?.(error),
      })
    );
    // Carry the last accepted document across activation and option changes.
    if (accepted.current) state.updateControlledSource(accepted.current, true);
    owned.current = state;
    setStream(state);
    return () => {
      owned.current = null;
      state.dispose();
    };
    // Equivalent option objects should not recreate the native session.
  }, [enabled, flags, tags]);

  useLayoutEffect(() => {
    const state = owned.current;
    if (!state) return;
    state.setPreview(props.preview ?? true);
    accepted.current = state.updateControlledSource(
      props.markdown,
      props.isAnimating === true
    );
  }, [
    stream,
    enabled,
    flags,
    tags,
    props.markdown,
    props.isAnimating,
    props.preview,
  ]);

  return stream;
}
