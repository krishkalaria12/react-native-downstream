import { createContext, useContext, useRef, type ReactNode } from 'react';
import {
  mergeDownstreamOptions,
  resolveDownstreamOptions,
  type DownstreamOptions,
  type ResolvedDownstreamOptions,
} from './downstreamOptions';

const DownstreamDefaults = createContext<ResolvedDownstreamOptions>({});

function sameOptions(
  previous: DownstreamOptions,
  next: DownstreamOptions
): boolean {
  const keys = Object.keys(previous) as Array<keyof DownstreamOptions>;
  return (
    keys.length === Object.keys(next).length &&
    keys.every((key) => previous[key] === next[key])
  );
}

export type DownstreamProviderProps = DownstreamOptions &
  Readonly<{ children: ReactNode }>;

/** App-wide defaults. Inner providers and individual views override outer values. */
export function DownstreamProvider({
  children,
  ...options
}: DownstreamProviderProps) {
  const inherited = useContext(DownstreamDefaults);
  const cached = useRef<{
    inherited: ResolvedDownstreamOptions;
    options: DownstreamOptions;
    value: ResolvedDownstreamOptions;
  } | null>(null);
  if (
    !cached.current ||
    cached.current.inherited !== inherited ||
    !sameOptions(cached.current.options, options)
  ) {
    cached.current = {
      inherited,
      options,
      value: mergeDownstreamOptions(
        inherited,
        resolveDownstreamOptions(options)
      ),
    };
  }
  return (
    <DownstreamDefaults.Provider value={cached.current.value}>
      {children}
    </DownstreamDefaults.Provider>
  );
}

export function useDownstreamDefaults(): ResolvedDownstreamOptions {
  return useContext(DownstreamDefaults);
}
