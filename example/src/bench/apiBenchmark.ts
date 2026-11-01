export const API_BENCHMARK_VERSION = 'api-plugins-v1';

export type BenchmarkSourceMode = 'string' | 'controller' | 'peer';
export type ApiBenchmarkConfig = Readonly<{
  candidate: string;
  benchmarkVersion?: string;
  syntaxHighlighting?: boolean;
  math?: boolean;
  preview?: boolean;
  trace?: boolean;
}>;

export type ApiBenchmarkConfiguration = Readonly<{
  sourceMode: BenchmarkSourceMode;
  plugins: readonly string[];
  preview: boolean | null;
  engines: Readonly<{ code: boolean; math: boolean }>;
}>;

/** Historical runs keep their original startup and result contract. */
export function usesApiBenchmarkProtocol(config: ApiBenchmarkConfig) {
  return (
    config.benchmarkVersion === API_BENCHMARK_VERSION ||
    config.candidate === 'downstream-string' ||
    config.candidate === 'downstream-controller'
  );
}

/** The same resolved policy drives rendering and the committed configuration receipt. */
export function apiBenchmarkPolicy(config: ApiBenchmarkConfig) {
  const sourceMode: BenchmarkSourceMode =
    config.candidate === 'downstream-string'
      ? 'string'
      : config.candidate.startsWith('downstream-')
        ? 'controller'
        : 'peer';
  const pluginKey = config.math
    ? config.syntaxHighlighting
      ? 'all'
      : 'math'
    : config.syntaxHighlighting
      ? 'code'
      : 'plain';
  const modern =
    config.candidate === 'downstream-string' ||
    config.candidate === 'downstream-controller';
  return {
    sourceMode,
    pluginKey,
    preview: sourceMode === 'peer' ? null : (config.preview ?? modern),
  } as const;
}

export function validateApiBenchmarkConfig(config: ApiBenchmarkConfig) {
  if (
    config.benchmarkVersion !== undefined &&
    config.benchmarkVersion !== API_BENCHMARK_VERSION
  )
    throw new Error(
      `Unsupported benchmark version: ${config.benchmarkVersion}`
    );
  if (config.trace && config.candidate === 'downstream-string')
    throw new Error(
      'Native diagnostic tracing is unavailable for the owned string session; use downstream-controller.'
    );
}

/** Controller samples receive chunks without a parent React source update. */
export function publishesSourceUpdates(candidate: string) {
  return candidate !== 'downstream-controller';
}
