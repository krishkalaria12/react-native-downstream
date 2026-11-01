import { describe, expect, it } from 'vitest';
import {
  API_BENCHMARK_VERSION,
  apiBenchmarkPolicy,
  publishesSourceUpdates,
  usesApiBenchmarkProtocol,
  validateApiBenchmarkConfig,
} from './apiBenchmark';

describe('API benchmark policy', () => {
  it('compares the two public sources with the same defaults', () => {
    expect(apiBenchmarkPolicy({ candidate: 'downstream-string' })).toEqual({
      sourceMode: 'string',
      pluginKey: 'plain',
      preview: true,
    });
    expect(apiBenchmarkPolicy({ candidate: 'downstream-controller' })).toEqual({
      sourceMode: 'controller',
      pluginKey: 'plain',
      preview: true,
    });
    expect(publishesSourceUpdates('downstream-string')).toBe(true);
    expect(publishesSourceUpdates('downstream-controller')).toBe(false);
  });

  it.each([
    [false, false, 'plain'],
    [true, false, 'code'],
    [false, true, 'math'],
    [true, true, 'all'],
  ] as const)(
    'selects code=%s and math=%s explicitly',
    (syntaxHighlighting, math, pluginKey) => {
      expect(
        apiBenchmarkPolicy({
          candidate: 'downstream-string',
          syntaxHighlighting,
          math,
          preview: false,
        })
      ).toMatchObject({ pluginKey, preview: false });
    }
  );

  it('keeps historical controller previews off and peer previews unspecified', () => {
    expect(
      apiBenchmarkPolicy({ candidate: 'downstream-default' }).preview
    ).toBe(false);
    expect(
      apiBenchmarkPolicy({ candidate: 'downstream-incremental' }).preview
    ).toBe(false);
    expect(
      apiBenchmarkPolicy({ candidate: 'streamdown', preview: true })
    ).toMatchObject({ sourceMode: 'peer', preview: null });
    expect(publishesSourceUpdates('downstream-default')).toBe(true);
    expect(usesApiBenchmarkProtocol({ candidate: 'downstream-default' })).toBe(
      false
    );
    expect(
      usesApiBenchmarkProtocol({
        candidate: 'streamdown',
        benchmarkVersion: API_BENCHMARK_VERSION,
      })
    ).toBe(true);
  });

  it('rejects unsupported versions and unsupported string-session tracing', () => {
    expect(() =>
      validateApiBenchmarkConfig({
        candidate: 'downstream-controller',
        benchmarkVersion: API_BENCHMARK_VERSION,
        trace: true,
      })
    ).not.toThrow();
    expect(() =>
      validateApiBenchmarkConfig({
        candidate: 'downstream-string',
        benchmarkVersion: 'future',
      })
    ).toThrow(/Unsupported benchmark version/);
    expect(() =>
      validateApiBenchmarkConfig({
        candidate: 'downstream-string',
        trace: true,
      })
    ).toThrow(/tracing is unavailable/);
  });
});
