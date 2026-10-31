export type LibraryName =
  | 'Downstream'
  | 'RN Streamdown'
  | 'Enriched Markdown'
  | 'Markdown Display'
  | 'Marked'
  | 'Markdown Renderer';

export type Metrics = {
  /** App process CPU time in seconds. */
  appCpu: number;
  /** UI (main) thread CPU time in seconds. */
  uiCpu: number;
  /** Peak memory increase in MiB. */
  memory: number;
  /** Janky frames where the app missed its deadline, in percent. */
  jank: number;
  /** Frame time p95 in ms. */
  p95: number;
  /** Frame time p99 in ms. */
  p99: number;
  /** Settle after last chunk in ms. */
  settle: number;
};

export type Workload = {
  id: string;
  label: string;
  description: string;
  replies: number;
  runs: string;
  results: Record<LibraryName, Metrics>;
};

export const libraries: LibraryName[] = [
  'Downstream',
  'RN Streamdown',
  'Enriched Markdown',
  'Markdown Display',
  'Marked',
  'Markdown Renderer',
];

export const workloads: Workload[] = [
  {
    id: 'ai-single',
    label: 'One AI reply',
    description:
      'Prose, a table, and a TypeScript block streaming into one view.',
    replies: 1,
    runs: '5 runs',
    results: {
      'Downstream': {
        appCpu: 6.56,
        uiCpu: 1.65,
        memory: 10.6,
        jank: 7.3,
        p95: 11.6,
        p99: 16.6,
        settle: 3,
      },
      'RN Streamdown': {
        appCpu: 11.83,
        uiCpu: 2.74,
        memory: 29.0,
        jank: 2.1,
        p95: 10.3,
        p99: 11.3,
        settle: 70,
      },
      'Enriched Markdown': {
        appCpu: 9.6,
        uiCpu: 2.84,
        memory: 16.6,
        jank: 3.4,
        p95: 10.2,
        p99: 11.4,
        settle: 15,
      },
      'Markdown Display': {
        appCpu: 11.1,
        uiCpu: 4.14,
        memory: 37.4,
        jank: 53.1,
        p95: 33.0,
        p99: 40.9,
        settle: 19,
      },
      'Marked': {
        appCpu: 13.81,
        uiCpu: 6.29,
        memory: 53.5,
        jank: 47.6,
        p95: 57.0,
        p99: 79.3,
        settle: 109,
      },
      'Markdown Renderer': {
        appCpu: 13.32,
        uiCpu: 6.24,
        memory: 45.6,
        jank: 58.9,
        p95: 63.4,
        p99: 79.1,
        settle: 71,
      },
    },
  },
  {
    id: 'ai-code-heavy',
    label: 'Code-heavy reply',
    description: 'Fourteen fenced code blocks in a single streaming response.',
    replies: 1,
    runs: '5 runs',
    results: {
      'Downstream': {
        appCpu: 6.34,
        uiCpu: 1.7,
        memory: 11.7,
        jank: 7.9,
        p95: 13.9,
        p99: 19.3,
        settle: 0,
      },
      'RN Streamdown': {
        appCpu: 13.21,
        uiCpu: 2.5,
        memory: 30.4,
        jank: 3.7,
        p95: 11.0,
        p99: 12.2,
        settle: 0,
      },
      'Enriched Markdown': {
        appCpu: 10.72,
        uiCpu: 2.28,
        memory: 17.6,
        jank: 2.8,
        p95: 10.3,
        p99: 12.9,
        settle: 0,
      },
      'Markdown Display': {
        appCpu: 15.49,
        uiCpu: 6.46,
        memory: 43.7,
        jank: 69.6,
        p95: 60.7,
        p99: 83.9,
        settle: 87,
      },
      'Marked': {
        appCpu: 18.93,
        uiCpu: 8.35,
        memory: 89.7,
        jank: 74.2,
        p95: 69.6,
        p99: 80.0,
        settle: 123,
      },
      'Markdown Renderer': {
        appCpu: 20.13,
        uiCpu: 9.39,
        memory: 72.3,
        jank: 71.8,
        p95: 86.6,
        p99: 125.9,
        settle: 0,
      },
    },
  },
  {
    id: 'ai-feature-matrix',
    label: 'Feature matrix',
    description: 'Math, code, and tables in every subset and ordering.',
    replies: 1,
    runs: '5 runs',
    results: {
      'Downstream': {
        appCpu: 10.64,
        uiCpu: 2.64,
        memory: 14.2,
        jank: 8.0,
        p95: 12.9,
        p99: 18.5,
        settle: 48,
      },
      'RN Streamdown': {
        appCpu: 21.72,
        uiCpu: 2.98,
        memory: 39.1,
        jank: 5.9,
        p95: 10.7,
        p99: 16.0,
        settle: 98,
      },
      'Enriched Markdown': {
        appCpu: 26.07,
        uiCpu: 4.21,
        memory: 26.4,
        jank: 2.6,
        p95: 10.3,
        p99: 11.6,
        settle: 71,
      },
      'Markdown Display': {
        appCpu: 33.4,
        uiCpu: 13.73,
        memory: 60.0,
        jank: 57.8,
        p95: 130.3,
        p99: 193.6,
        settle: 323,
      },
      'Marked': {
        appCpu: 32.13,
        uiCpu: 14.63,
        memory: 109.8,
        jank: 45.8,
        p95: 134.2,
        p99: 306.6,
        settle: 0,
      },
      'Markdown Renderer': {
        appCpu: 36.5,
        uiCpu: 17.62,
        memory: 160.0,
        jank: 55.4,
        p95: 166.4,
        p99: 483.7,
        settle: 635,
      },
    },
  },
  {
    id: 'ai-concurrent-5',
    label: 'Five replies at once',
    description:
      'Five AI replies streaming concurrently, including the feature matrix.',
    replies: 5,
    runs: '5 runs',
    results: {
      'Downstream': {
        appCpu: 23.33,
        uiCpu: 5.29,
        memory: 23.4,
        jank: 2.3,
        p95: 10.9,
        p99: 14.0,
        settle: 0,
      },
      'RN Streamdown': {
        appCpu: 48.86,
        uiCpu: 7.37,
        memory: 72.4,
        jank: 2.2,
        p95: 10.4,
        p99: 14.1,
        settle: 0,
      },
      'Enriched Markdown': {
        appCpu: 44.76,
        uiCpu: 9.58,
        memory: 37.7,
        jank: 4.4,
        p95: 11.1,
        p99: 16.7,
        settle: 73,
      },
      'Markdown Display': {
        appCpu: 53.17,
        uiCpu: 20.89,
        memory: 84.5,
        jank: 64.1,
        p95: 115.0,
        p99: 159.1,
        settle: 122,
      },
      'Marked': {
        appCpu: 62.87,
        uiCpu: 26.52,
        memory: 100.6,
        jank: 76.5,
        p95: 131.2,
        p99: 196.3,
        settle: 65,
      },
      'Markdown Renderer': {
        appCpu: 63.43,
        uiCpu: 29.35,
        memory: 167.5,
        jank: 54.0,
        p95: 308.0,
        p99: 1002.3,
        settle: 300,
      },
    },
  },
  {
    id: 'ai-huge-single',
    label: 'One huge reply',
    description: 'Sixty points of mixed features in a single response.',
    replies: 1,
    runs: '1 run',
    results: {
      'Downstream': {
        appCpu: 14.72,
        uiCpu: 4.19,
        memory: 12.9,
        jank: 4.4,
        p95: 11.1,
        p99: 11.8,
        settle: 0,
      },
      'RN Streamdown': {
        appCpu: 32.32,
        uiCpu: 5.68,
        memory: 68.1,
        jank: 2.8,
        p95: 10.7,
        p99: 11.7,
        settle: 165,
      },
      'Enriched Markdown': {
        appCpu: 34.78,
        uiCpu: 7.3,
        memory: 26.1,
        jank: 3.5,
        p95: 10.9,
        p99: 12.0,
        settle: 94,
      },
      'Markdown Display': {
        appCpu: 64.6,
        uiCpu: 29.41,
        memory: 85.5,
        jank: 79.8,
        p95: 193.0,
        p99: 262.8,
        settle: 485,
      },
      'Marked': {
        appCpu: 70.27,
        uiCpu: 32.22,
        memory: 108.8,
        jank: 83.7,
        p95: 223.7,
        p99: 406.8,
        settle: 575,
      },
      'Markdown Renderer': {
        appCpu: 73.42,
        uiCpu: 34.88,
        memory: 157.8,
        jank: 77.6,
        p95: 307.0,
        p99: 738.9,
        settle: 0,
      },
    },
  },
  {
    id: 'ai-huge-concurrent-5',
    label: 'Five huge replies',
    description: 'Five simultaneous sixty-point replies with mixed features.',
    replies: 5,
    runs: '1 run',
    results: {
      'Downstream': {
        appCpu: 36.4,
        uiCpu: 5.82,
        memory: 39.1,
        jank: 0.7,
        p95: 9.8,
        p99: 12.4,
        settle: 0,
      },
      'RN Streamdown': {
        appCpu: 93.68,
        uiCpu: 13.39,
        memory: 145.6,
        jank: 17.9,
        p95: 18.3,
        p99: 21.2,
        settle: 265,
      },
      'Enriched Markdown': {
        appCpu: 74.92,
        uiCpu: 12.56,
        memory: 76.5,
        jank: 12.1,
        p95: 16.5,
        p99: 21.5,
        settle: 26,
      },
      'Markdown Display': {
        appCpu: 99.65,
        uiCpu: 41.16,
        memory: 179.4,
        jank: 71.1,
        p95: 535.1,
        p99: 908.1,
        settle: 1431,
      },
      'Marked': {
        appCpu: 93.82,
        uiCpu: 37.81,
        memory: 120.6,
        jank: 72.3,
        p95: 281.1,
        p99: 377.0,
        settle: 38,
      },
      'Markdown Renderer': {
        appCpu: 113.09,
        uiCpu: 53.66,
        memory: 489.6,
        jank: 71.7,
        p95: 306.4,
        p99: 1794.1,
        settle: 13461,
      },
    },
  },
];

export type Relative = {
  library: LibraryName;
  appCpu: string;
  uiCpu: string;
  memory: string;
  jank: string;
  p99: string;
  significant: boolean;
};

export const relativeByWorkload: Record<string, Relative[]> = {
  'ai-single': [
    {
      library: 'RN Streamdown',
      appCpu: '1.80×',
      uiCpu: '1.66×',
      memory: '2.75×*',
      jank: '0.28×*',
      p99: '0.68×*',
      significant: true,
    },
    {
      library: 'Enriched Markdown',
      appCpu: '1.46×',
      uiCpu: '1.72×*',
      memory: '1.57×*',
      jank: '0.46×*',
      p99: '0.69×*',
      significant: true,
    },
    {
      library: 'Markdown Display',
      appCpu: '1.69×*',
      uiCpu: '2.50×*',
      memory: '3.55×*',
      jank: '7.22×*',
      p99: '2.47×*',
      significant: true,
    },
    {
      library: 'Marked',
      appCpu: '2.11×*',
      uiCpu: '3.81×*',
      memory: '5.07×*',
      jank: '6.48×*',
      p99: '4.78×*',
      significant: true,
    },
    {
      library: 'Markdown Renderer',
      appCpu: '2.03×*',
      uiCpu: '3.77×*',
      memory: '4.32×*',
      jank: '8.03×*',
      p99: '4.78×*',
      significant: true,
    },
  ],
  'ai-code-heavy': [
    {
      library: 'RN Streamdown',
      appCpu: '2.08×*',
      uiCpu: '1.47×*',
      memory: '2.60×*',
      jank: '0.46×*',
      p99: '0.63×*',
      significant: true,
    },
    {
      library: 'Enriched Markdown',
      appCpu: '1.69×*',
      uiCpu: '1.34×',
      memory: '1.51×',
      jank: '0.36×',
      p99: '0.67×',
      significant: true,
    },
    {
      library: 'Markdown Display',
      appCpu: '2.44×*',
      uiCpu: '3.80×*',
      memory: '3.74×*',
      jank: '8.77×*',
      p99: '4.34×*',
      significant: true,
    },
    {
      library: 'Marked',
      appCpu: '2.99×*',
      uiCpu: '4.91×*',
      memory: '7.67×*',
      jank: '9.35×*',
      p99: '4.14×*',
      significant: true,
    },
    {
      library: 'Markdown Renderer',
      appCpu: '3.18×*',
      uiCpu: '5.52×*',
      memory: '6.18×*',
      jank: '9.05×*',
      p99: '6.52×*',
      significant: true,
    },
  ],
  'ai-feature-matrix': [
    {
      library: 'RN Streamdown',
      appCpu: '2.04×*',
      uiCpu: '1.13×*',
      memory: '2.75×*',
      jank: '0.74×',
      p99: '0.86×*',
      significant: true,
    },
    {
      library: 'Enriched Markdown',
      appCpu: '2.45×*',
      uiCpu: '1.60×*',
      memory: '1.86×*',
      jank: '0.32×*',
      p99: '0.63×*',
      significant: true,
    },
    {
      library: 'Markdown Display',
      appCpu: '3.14×*',
      uiCpu: '5.21×*',
      memory: '4.22×*',
      jank: '7.24×*',
      p99: '10.46×*',
      significant: true,
    },
    {
      library: 'Marked',
      appCpu: '3.02×*',
      uiCpu: '5.55×*',
      memory: '7.72×*',
      jank: '5.74×*',
      p99: '16.56×*',
      significant: true,
    },
    {
      library: 'Markdown Renderer',
      appCpu: '3.43×*',
      uiCpu: '6.69×*',
      memory: '11.25×*',
      jank: '6.95×*',
      p99: '26.13×*',
      significant: true,
    },
  ],
  'ai-concurrent-5': [
    {
      library: 'RN Streamdown',
      appCpu: '2.09×*',
      uiCpu: '1.39×*',
      memory: '3.09×*',
      jank: '0.94×',
      p99: '1.01×',
      significant: true,
    },
    {
      library: 'Enriched Markdown',
      appCpu: '1.92×',
      uiCpu: '1.81×*',
      memory: '1.61×*',
      jank: '1.87×',
      p99: '1.19×',
      significant: true,
    },
    {
      library: 'Markdown Display',
      appCpu: '2.28×*',
      uiCpu: '3.95×*',
      memory: '3.61×*',
      jank: '27.39×*',
      p99: '11.34×*',
      significant: true,
    },
    {
      library: 'Marked',
      appCpu: '2.69×*',
      uiCpu: '5.02×*',
      memory: '4.30×*',
      jank: '32.70×*',
      p99: '14.00×*',
      significant: true,
    },
    {
      library: 'Markdown Renderer',
      appCpu: '2.72×*',
      uiCpu: '5.55×*',
      memory: '7.16×*',
      jank: '23.07×*',
      p99: '71.46×*',
      significant: true,
    },
  ],
  'ai-huge-single': [
    {
      library: 'RN Streamdown',
      appCpu: '2.20×',
      uiCpu: '1.36×',
      memory: '5.28×',
      jank: '0.65×',
      p99: '1.00×',
      significant: false,
    },
    {
      library: 'Enriched Markdown',
      appCpu: '2.36×',
      uiCpu: '1.74×',
      memory: '2.02×',
      jank: '0.80×',
      p99: '1.02×',
      significant: false,
    },
    {
      library: 'Markdown Display',
      appCpu: '4.39×',
      uiCpu: '7.02×',
      memory: '6.63×',
      jank: '18.29×',
      p99: '22.30×',
      significant: false,
    },
    {
      library: 'Marked',
      appCpu: '4.77×',
      uiCpu: '7.69×',
      memory: '8.45×',
      jank: '19.18×',
      p99: '34.51×',
      significant: false,
    },
    {
      library: 'Markdown Renderer',
      appCpu: '4.99×',
      uiCpu: '8.32×',
      memory: '12.25×',
      jank: '17.79×',
      p99: '62.69×',
      significant: false,
    },
  ],
  'ai-huge-concurrent-5': [
    {
      library: 'RN Streamdown',
      appCpu: '2.57×',
      uiCpu: '2.30×',
      memory: '3.72×',
      jank: '26.74×',
      p99: '1.71×',
      significant: false,
    },
    {
      library: 'Enriched Markdown',
      appCpu: '2.06×',
      uiCpu: '2.16×',
      memory: '1.96×',
      jank: '18.03×',
      p99: '1.73×',
      significant: false,
    },
    {
      library: 'Markdown Display',
      appCpu: '2.74×',
      uiCpu: '7.08×',
      memory: '4.59×',
      jank: '106.29×',
      p99: '73.21×',
      significant: false,
    },
    {
      library: 'Marked',
      appCpu: '2.58×',
      uiCpu: '6.50×',
      memory: '3.08×',
      jank: '108.08×',
      p99: '30.39×',
      significant: false,
    },
    {
      library: 'Markdown Renderer',
      appCpu: '3.11×',
      uiCpu: '9.22×',
      memory: '12.52×',
      jank: '107.20×',
      p99: '144.63×',
      significant: false,
    },
  ],
};

export const featureMatrix: {
  feature: string;
  downstream: string;
  streamdown: string;
  enriched: string;
  display: string;
  marked: string;
  renderer: string;
}[] = [
  {
    feature: 'Native text rendering',
    downstream: 'yes',
    streamdown: 'yes',
    enriched: 'yes',
    display: 'no',
    marked: 'no',
    renderer: 'no',
  },
  {
    feature: 'Append-only stream API',
    downstream: 'yes',
    streamdown: 'no',
    enriched: 'no',
    display: 'no',
    marked: 'no',
    renderer: 'no',
  },
  {
    feature: 'Reuses finished blocks while streaming',
    downstream: 'yes',
    streamdown: 'no',
    enriched: 'no',
    display: 'no',
    marked: 'no',
    renderer: 'no',
  },
  {
    feature: 'Repairs unfinished Markdown mid-stream',
    downstream: 'per syntax',
    streamdown: 'remend',
    enriched: 'no',
    display: 'no',
    marked: 'no',
    renderer: 'no',
  },
  {
    feature: 'Streaming reveal animation',
    downstream: 'yes',
    streamdown: 'yes',
    enriched: 'yes',
    display: 'no',
    marked: 'no',
    renderer: 'no',
  },
  {
    feature: 'Streaming caret',
    downstream: 'yes',
    streamdown: 'no',
    enriched: 'no',
    display: 'no',
    marked: 'no',
    renderer: 'no',
  },
  {
    feature: 'Syntax highlighting',
    downstream: 'built in',
    streamdown: 'build-time',
    enriched: 'build-time',
    display: 'no',
    marked: 'custom',
    renderer: 'no',
  },
  {
    feature: 'Line numbers, filename, download, wrap, expand',
    downstream: 'yes',
    streamdown: 'copy only',
    enriched: 'copy only',
    display: 'no',
    marked: 'no',
    renderer: 'no',
  },
  {
    feature: 'Table copy, export and full-screen',
    downstream: 'yes',
    streamdown: 'no',
    enriched: 'no',
    display: 'no',
    marked: 'no',
    renderer: 'no',
  },
  {
    feature: 'Footnotes',
    downstream: 'yes',
    streamdown: 'no',
    enriched: 'no',
    display: 'no',
    marked: 'no',
    renderer: 'no',
  },
  {
    feature: 'Alerts / admonitions',
    downstream: 'yes',
    streamdown: 'yes',
    enriched: 'yes',
    display: 'no',
    marked: 'no',
    renderer: 'no',
  },
  {
    feature: 'Math',
    downstream: 'optional package',
    streamdown: 'yes',
    enriched: 'yes',
    display: 'no',
    marked: 'custom',
    renderer: 'no',
  },
  {
    feature: 'Collapsible details',
    downstream: 'yes',
    streamdown: 'no',
    enriched: 'no',
    display: 'no',
    marked: 'no',
    renderer: 'no',
  },
  {
    feature: 'Your own React components in fences and tags',
    downstream: 'yes',
    streamdown: 'no',
    enriched: 'no',
    display: 'per-node',
    marked: 'per-node',
    renderer: 'per-node',
  },
];

/** iOS simulator app CPU medians in seconds (animations off), from REPORT_IOS_SIMULATOR.md. */
export const iosAppCpu: {
  id: string;
  label: string;
  results: Record<LibraryName, number>;
}[] = [
  {
    id: 'ai-single',
    label: 'One AI reply',
    results: {
      'Downstream': 2.33,
      'RN Streamdown': 3.29,
      'Enriched Markdown': 3.43,
      'Markdown Display': 4.96,
      'Marked': 5.1,
      'Markdown Renderer': 4.26,
    },
  },
  {
    id: 'ai-code-heavy',
    label: 'Code-heavy reply',
    results: {
      'Downstream': 2.48,
      'RN Streamdown': 3.44,
      'Enriched Markdown': 3.42,
      'Markdown Display': 6.39,
      'Marked': 6.66,
      'Markdown Renderer': 6.57,
    },
  },
  {
    id: 'ai-feature-matrix',
    label: 'Feature matrix',
    results: {
      'Downstream': 3.91,
      'RN Streamdown': 7.06,
      'Enriched Markdown': 6.13,
      'Markdown Display': 12.94,
      'Marked': 15.4,
      'Markdown Renderer': 13.21,
    },
  },
  {
    id: 'ai-concurrent-5',
    label: 'Five replies at once',
    results: {
      'Downstream': 10.74,
      'RN Streamdown': 17.32,
      'Enriched Markdown': 14.94,
      'Markdown Display': 28.32,
      'Marked': 24.22,
      'Markdown Renderer': 24.99,
    },
  },
];

export type MetricKey = keyof Metrics;

export const metrics: {
  key: MetricKey;
  label: string;
  unit: string;
  decimals: number;
  about: string;
  relative: boolean;
}[] = [
  {
    key: 'appCpu',
    label: 'App CPU',
    unit: 's',
    decimals: 2,
    about:
      'CPU time across every thread in the app process while the reply streams.',
    relative: true,
  },
  {
    key: 'uiCpu',
    label: 'UI thread CPU',
    unit: 's',
    decimals: 2,
    about:
      'CPU time on the main thread, the one that also has to handle touches and scrolling.',
    relative: true,
  },
  {
    key: 'memory',
    label: 'Peak memory',
    unit: 'MiB',
    decimals: 1,
    about:
      'Highest anonymous RSS plus swap during the run, minus the value when streaming started.',
    relative: true,
  },
  {
    key: 'jank',
    label: 'Janky frames',
    unit: '%',
    decimals: 1,
    about:
      'Share of frames that Android itself blames on the app for missing their deadline.',
    relative: true,
  },
  {
    key: 'p99',
    label: 'Frame time p99',
    unit: 'ms',
    decimals: 1,
    about:
      'The slowest 1% of frames, from CPU start to GPU done. 8.3 ms is one refresh at 120 Hz.',
    relative: true,
  },
  {
    key: 'settle',
    label: 'Settle',
    unit: 'ms',
    decimals: 0,
    about: 'Time from the last delivered chunk to the last frame the app drew.',
    relative: false,
  },
];

export const workload = (id: string) =>
  workloads.find((item) => item.id === id)!;
