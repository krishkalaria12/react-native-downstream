import { useLayoutEffect, useRef, useState } from 'react';
import {
  createMarkdownStream,
  getStreamState,
  type StreamState,
} from './createMarkdownStream';
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

  useLayoutEffect(() => {
    if (!enabled) return;
    const state = getStreamState(
      createMarkdownStream({
        onError: (error) => latest.current.onError?.(error),
      })
    );
    // Carry the last accepted document across activation.
    if (accepted.current) state.updateControlledSource(accepted.current, true);
    owned.current = state;
    setStream(state);
    return () => {
      owned.current = null;
      state.dispose();
    };
  }, [enabled]);

  useLayoutEffect(() => {
    const state = owned.current;
    if (!state) return;
    accepted.current = state.updateControlledSource(
      props.markdown,
      props.isAnimating === true
    );
  }, [stream, enabled, props.markdown, props.isAnimating]);

  return stream;
}
