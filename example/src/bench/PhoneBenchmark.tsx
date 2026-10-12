import {
  Component,
  Fragment,
  useEffect,
  useLayoutEffect,
  useRef,
  useState,
} from 'react';
import type { ReactNode } from 'react';
import {
  ScrollView,
  StatusBar,
  StyleSheet,
  Text,
  View,
} from 'react-native';
import { createMarkdownStream, DownstreamMarkdown } from 'react-native-downstream';
import type { MarkdownStreamController } from 'react-native-downstream';
import { createMarkdownStreamWithMode, getStreamState } from '../../../src/createMarkdownStream';
import { setStreamTracing, subscribeStreamTrace } from '../../../src/streamTrace';
import type { StreamTraceEvent } from '../../../src/streamTrace';
import { StreamdownText } from 'react-native-streamdown';
import { EnrichedMarkdownText } from 'react-native-enriched-markdown';
import MarkdownDisplay from 'react-native-markdown-display';
import Marked from 'react-native-marked';
import MarkdownRenderer from 'react-native-markdown-renderer';
import { WebView } from 'react-native-webview';
import {
  phoneFixedChunks,
  phoneSourceInfo,
  phoneWorkloads,
} from './phoneWorkloads';
import type { PhoneChunk, PhoneWorkload } from './phoneWorkloads';

const downstreamTheme = { colors: { foreground: '#111111', mutedForeground: '#555555', primary: '#1565c0', muted: '#f4f4f4', border: '#aaaaaa' }, maxFontSizeMultiplier: 1 };
const HOST = 'http://127.0.0.1:8765';
const SETTLE_MS = 2000;
const TRACE_EVENT_LIMIT = 40000;
const textStyle = { fontSize: 16, lineHeight: 24, color: '#111111' };
const nativeMarkdownStyle = {
  paragraph: { ...textStyle, marginTop: 0, marginBottom: 12 },
  list: textStyle,
  blockquote: textStyle,
  code: { fontSize: 16, color: '#111111' },
  codeBlock: { ...textStyle, backgroundColor: '#f4f4f4' },
  table: textStyle,
};
// All fourteen public enriched-markdown token keys, including inherited colors.
// Its optional native highlighting module is selected when building the APK.
const githubLightSyntaxColors = {
  keyword: '#d73a49',
  string: '#032f62',
  number: '#005cc5',
  constant: '#005cc5',
  property: '#005cc5',
  comment: '#6a737d',
  function: '#6f42c1',
  type: '#e36209',
  tag: '#22863a',
  attribute: '#005cc5',
  operator: '#111111',
  punctuation: '#111111',
  variable: '#111111',
  embedded: '#111111',
};
const coloredNativeMarkdownStyle = {
  ...nativeMarkdownStyle,
  codeBlock: {
    ...nativeMarkdownStyle.codeBlock,
    syntaxColors: githubLightSyntaxColors,
  },
};
const jsMarkdownStyle = {
  body: textStyle,
  text: textStyle,
  paragraph: { marginTop: 0, marginBottom: 12 },
  code_inline: textStyle,
  code_block: { ...textStyle, backgroundColor: '#f4f4f4' },
  fence: { ...textStyle, backgroundColor: '#f4f4f4' },
};
const markedTheme = { colors: { text: '#111111', code: '#f4f4f4', link: '#1565c0', border: '#aaaaaa' } };
const markedStyle = {
  text: textStyle,
  li: textStyle,
  codespan: textStyle,
  codeText: textStyle,
  code: { backgroundColor: '#f4f4f4' },
  paragraph: { paddingVertical: 0, marginTop: 0, marginBottom: 12 },
};
const rendererStyle = {
  text: textStyle,
  paragraph: { marginTop: 0, marginBottom: 12 },
  codeInline: { ...textStyle, fontFamily: 'monospace' },
  codeBlock: { ...textStyle, fontFamily: 'monospace', backgroundColor: '#f4f4f4' },
};

export type PhoneCandidate =
  | 'downstream-default'
  | 'downstream-full'
  | 'downstream-incremental'
  | 'streamdown'
  | 'enriched'
  | 'markdown-display'
  | 'marked'
  | 'markdown-renderer'
  | 'web-streamdown';

interface RunConfig {
  candidate: PhoneCandidate;
  workload: string;
  runId: string;
  mode: 'stream' | 'static' | 'correctness';
  warmup?: boolean;
  trace?: boolean;
  diagnostic?: boolean;
  syntaxHighlighting?: boolean;
  endpoint?: string;
  source?: string;
  chunks?: PhoneChunk[] | string[];
  dueMs?: number[];
}
interface Snapshot {
  source: string;
  revision: number;
  finished: boolean;
  capture?: boolean;
}
interface RunSamples {
  deliveries: { revision: number; dueMs: number; actualMs: number; latenessMs: number; codepoints: number }[];
  rafSamples: { atMs: number; gapMs: number }[];
  commits: { revision: number; atMs: number }[];
  webCommits: Record<string, unknown>[];
  errors: string[];
  startAt: number;
  deliveryDurationMs: number;
}
interface TraceSamples {
  enabled: true;
  sessionId: string;
  eventLimit: number;
  droppedEvents: number;
  intervalComplete: boolean;
  events: StreamTraceEvent[];
}
const now = () => (globalThis as { performance?: { now: () => number } }).performance?.now() ?? Date.now();

async function post(endpoint: string, path: string, value: unknown) {
  const response = await fetch(`${endpoint}${path}`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(value),
  });
  if (!response.ok) throw new Error(`${path}: HTTP ${response.status}`);
}

async function getWorkload(config: RunConfig): Promise<PhoneWorkload> {
  let workload: PhoneWorkload;
  if (config.source !== undefined || config.chunks !== undefined) {
    const chunks = config.chunks?.map((chunk, index) =>
      typeof chunk === 'string'
        ? { text: chunk, dueMs: config.dueMs?.[index] ?? index * 32 }
        : chunk
    );
    const source = config.source ?? chunks!.map((chunk) => chunk.text).join('');
    workload = { source, chunks: chunks ?? phoneFixedChunks(source) };
  } else if (config.workload === 'ai-replay') {
    const response = await fetch(`${config.endpoint ?? HOST}/ai-response.json`);
    if (!response.ok) throw new Error(`AI fixture: HTTP ${response.status}`);
    workload = (await response.json()) as PhoneWorkload;
  } else {
    const known = phoneWorkloads[config.workload];
    if (!known) throw new Error(`Unknown workload: ${config.workload}`);
    workload = known;
  }
  if (workload.chunks.map((chunk) => chunk.text).join('') !== workload.source) {
    throw new Error('Recorded chunks do not reproduce the exact source');
  }
  let previousDue = -1;
  for (const chunk of workload.chunks) {
    if (!Number.isFinite(chunk.dueMs) || chunk.dueMs < 0 || chunk.dueMs < previousDue) {
      throw new Error('Chunk due times must be finite, nonnegative and monotonic');
    }
    previousDue = chunk.dueMs;
  }
  if (config.mode !== 'stream') {
    return { source: workload.source, chunks: [{ text: workload.source, dueMs: 0 }] };
  }
  return workload;
}

class RendererBoundary extends Component<{
  children: ReactNode;
  onError: (error: string) => void;
}, { failed: boolean }> {
  state = { failed: false };
  static getDerivedStateFromError() { return { failed: true }; }
  componentDidCatch(error: Error) { this.props.onError(String(error)); }
  render() {
    return this.state.failed ? <Text accessibilityLabel="renderer-error">Renderer failed</Text> : this.props.children;
  }
}

function Candidate({
  config,
  snapshot,
  controller,
  onWebReady,
  onWebMessage,
  onError,
}: {
  config: RunConfig;
  snapshot: Snapshot;
  controller: MarkdownStreamController | null;
  onWebReady: () => void;
  onWebMessage: (message: Record<string, unknown>) => void;
  onError: (error: string) => void;
}) {
  const web = useRef<WebView<object>>(null);
  const webReady = useRef(false);
  const syntaxHighlighting = config.syntaxHighlighting ?? false;
  const markdownStyle = syntaxHighlighting ? coloredNativeMarkdownStyle : nativeMarkdownStyle;
  useEffect(() => {
    if (config.candidate === 'web-streamdown' && webReady.current) {
      web.current?.injectJavaScript(snapshot.capture ? 'window.phoneCapture();true;' : `window.phoneUpdate(${JSON.stringify({ ...snapshot, syntaxHighlighting })});true;`);
    }
  }, [config.candidate, syntaxHighlighting, snapshot]);

  if (config.candidate === 'web-streamdown') {
    return (
      <WebView<object>
        ref={web}
        testID="phone-benchmark-web-document"
        source={{ uri: `${config.endpoint ?? HOST}/web/index.html` }}
        style={styles.web}
        onError={(event) => onError(`WebView: ${event.nativeEvent.description}`)}
        onHttpError={(event) => onError(`WebView HTTP ${event.nativeEvent.statusCode}`)}
        onMessage={(event) => {
          try {
            const message = JSON.parse(event.nativeEvent.data) as Record<string, unknown>;
            if (message.type === 'ready') {
              webReady.current = true;
              onWebReady();
            } else if (message.type === 'error') {
              onError(`Web runtime: ${String(message.message)}`);
            } else {
              onWebMessage(message);
            }
          } catch (error) { onError(`WebView protocol: ${String(error)}`); }
        }}
        javaScriptEnabled
        domStorageEnabled={false}
        androidLayerType="hardware"
        mixedContentMode="never"
      />
    );
  }
  if (config.candidate === 'marked') {
    return (
      <Marked
        value={snapshot.source}
        styles={markedStyle}
        theme={markedTheme}
        selectable={false}
        flatListProps={{
          testID: 'phone-benchmark-document',
          style: styles.viewport,
          contentContainerStyle: styles.document,
        }}
      />
    );
  }
  let document: ReactNode;
  switch (config.candidate) {
    case 'downstream-default':
    case 'downstream-full':
    case 'downstream-incremental':
      document = controller ? (
        <DownstreamMarkdown
          stream={controller}
          androidParagraphLayout={config.candidate === 'downstream-incremental' ? 'incremental' : 'canonical'}
          markdownStyle={markdownStyle}
          theme={downstreamTheme}
          animated={false}
          syntaxHighlighting={syntaxHighlighting}
          lineNumbers={false}
          codeBlockMaxHeight={0}
          controls={false}
          math={false}
          selectable={false}
          onError={(error) => onError(JSON.stringify(error))}
        />
      ) : null;
      break;
    case 'streamdown':
      document = <StreamdownText markdown={snapshot.source} flavor="github" markdownStyle={markdownStyle} streamingAnimation={false} selectable={false} allowFontScaling={false} md4cFlags={{ latexMath: false }} />;
      break;
    case 'enriched':
      document = <EnrichedMarkdownText markdown={snapshot.source} flavor="github" markdownStyle={markdownStyle} streamingAnimation={false} selectable={false} allowFontScaling={false} md4cFlags={{ latexMath: false }} />;
      break;
    case 'markdown-display':
      document = <MarkdownDisplay style={jsMarkdownStyle}>{snapshot.source}</MarkdownDisplay>;
      break;
    case 'markdown-renderer':
      document = <MarkdownRenderer style={rendererStyle} maxTopLevelChildren={null}>{snapshot.source}</MarkdownRenderer>;
      break;
    default:
      throw new Error(`Unsupported candidate: ${String(config.candidate)}`);
  }
  return <ScrollView testID="phone-benchmark-document" style={styles.viewport} contentContainerStyle={styles.document}>{document}</ScrollView>;
}

/** One process launch runs one sample; final output stays mounted for native capture. */
export default function PhoneBenchmark() {
  const [config, setConfig] = useState<RunConfig | null>(null);
  const [snapshot, setSnapshot] = useState<Snapshot>({ source: '', revision: 0, finished: false });
  const [status, setStatus] = useState('Waiting for benchmark configuration');
  const controller = useRef<MarkdownStreamController | null>(null);
  const samples = useRef<RunSamples>({ deliveries: [], rafSamples: [], commits: [], webCommits: [], errors: [], startAt: 0, deliveryDurationMs: 0 });
  const webResolve = useRef<(() => void) | null>(null);
  const stopTraceOnError = useRef<(() => void) | null>(null);

  useLayoutEffect(() => {
    if (snapshot.revision > 0 && samples.current.startAt > 0) {
      samples.current.commits.push({ revision: snapshot.revision, atMs: now() - samples.current.startAt });
    }
  }, [snapshot]);

  useEffect(() => {
    let disposed = false;
    let measuring = false;
    let frame = 0;
    const timers = new Set<ReturnType<typeof setTimeout>>();
    const delay = (ms: number) => new Promise<void>((resolve) => {
      const timer = setTimeout(() => { timers.delete(timer); resolve(); }, ms);
      timers.add(timer);
    });
    let activeConfig: RunConfig | null = null;
    let trace: TraceSamples | undefined;
    let unsubscribeTrace: (() => void) | undefined;
    const stopTrace = (intervalComplete: boolean) => {
      if (!unsubscribeTrace) return;
      if (trace) trace.intervalComplete = intervalComplete;
      try {
        if (controller.current) setStreamTracing(controller.current, false);
      } catch (error) {
        if (trace) trace.intervalComplete = false;
        samples.current.errors.push(`Trace cleanup: ${String(error)}`);
      } finally {
        unsubscribeTrace();
        unsubscribeTrace = undefined;
      }
    };
    const reportError = (error: unknown) => {
      samples.current.errors.push(String(error));
      stopTrace(false);
    };
    stopTraceOnError.current = () => stopTrace(false);
    async function execute() {
      while (!disposed) {
        try {
          const response = await fetch(`${HOST}/config`);
          if (!response.ok) throw new Error(`Config: HTTP ${response.status}`);
          const next = (await response.json()) as RunConfig | null;
          if (next?.runId) { activeConfig = next; break; }
        } catch (error) { setStatus(`Waiting: ${String(error)}`); }
        await delay(500);
      }
      if (disposed || !activeConfig) return;
      const run = activeConfig;
      const endpoint = run.endpoint ?? HOST;
      const workload = await getWorkload(run);
      if (disposed) return;
      if (run.trace && !run.candidate.startsWith('downstream-')) {
        throw new Error('Diagnostic tracing requires a Downstream candidate');
      }
      const webReady = new Promise<void>((resolve) => { webResolve.current = resolve; });
      if (run.candidate.startsWith('downstream-')) {
        const options = { math: false, onError: (error: unknown) => reportError(JSON.stringify(error)) };
        controller.current = run.candidate === 'downstream-default'
          ? createMarkdownStream(options)
          : createMarkdownStreamWithMode(options, run.candidate === 'downstream-full' ? 'full' : 'incremental');
        if (run.trace) {
          const sessionId = getStreamState(controller.current).sessionId;
          const collection: TraceSamples = {
            enabled: true, sessionId, eventLimit: TRACE_EVENT_LIMIT,
            droppedEvents: 0, intervalComplete: false, events: [],
          };
          trace = collection;
          // Trace delivery stays in memory; one result POST occurs after the interval.
          unsubscribeTrace = subscribeStreamTrace((entry) => {
            if (entry.sessionId !== sessionId) return;
            if (collection.events.length < collection.eventLimit) collection.events.push(entry);
            else collection.droppedEvents += 1;
          });
          setStreamTracing(controller.current, true);
        }
      }
      setConfig(run);
      const event = (type: string, extra = {}) => post(endpoint, '/event', {
        runId: run.runId, candidate: run.candidate, workload: run.workload, mode: run.mode,
        type, appTimeMs: now(), wallTimeMs: Date.now(), ...extra,
      });
      await event('mounted');
      if (run.candidate === 'web-streamdown') {
        await Promise.race([webReady, delay(30000).then(() => { throw new Error('Web renderer did not become ready'); })]);
      }
      // Common empty-renderer mount settling. Startup is outside the measured run.
      await delay(1000);
      if (disposed) return;
      await event('start');
      const data = samples.current;
      data.startAt = now();
      measuring = true;
      let previousFrame = data.startAt;
      const observeFrame = () => {
        if (!measuring || disposed) return;
        const at = now();
        data.rafSamples.push({ atMs: at - data.startAt, gapMs: at - previousFrame });
        previousFrame = at;
        frame = requestAnimationFrame(observeFrame);
      };
      frame = requestAnimationFrame(observeFrame);
      let source = '';
      for (let index = 0; index < workload.chunks.length; index += 1) {
        const chunk = workload.chunks[index]!;
        const wait = chunk.dueMs - (now() - data.startAt);
        if (wait > 0) await delay(wait);
        if (disposed) return;
        const actualMs = now() - data.startAt;
        source += chunk.text;
        const revision = index + 1;
        const finished = revision === workload.chunks.length;
        data.deliveries.push({ revision, dueMs: chunk.dueMs, actualMs, latenessMs: actualMs - chunk.dueMs, codepoints: Array.from(chunk.text).length });
        controller.current?.append(chunk.text);
        if (finished) controller.current?.finish();
        setSnapshot({ source, revision, finished });
      }
      if (workload.chunks.length === 0) { controller.current?.finish(); setSnapshot({ source, revision: 0, finished: true }); }
      data.deliveryDurationMs = now() - data.startAt;
      // All renderers get the same settle budget; native readiness is not a speed metric.
      void event('delivery-end').catch(reportError);
      await delay(SETTLE_MS);
      const totalDurationMs = now() - data.startAt;
      measuring = false;
      cancelAnimationFrame(frame);
      stopTrace(data.errors.length === 0);
      await event('end', { totalDurationMs });
      if (disposed) return;
      if (run.candidate === 'web-streamdown') {
        setSnapshot((current) => ({ ...current, capture: true }));
        await delay(250);
      }
      await post(endpoint, '/result', {
        schemaVersion: 1,
        ...run,
        diagnostic: Boolean(run.trace || run.diagnostic),
        ...(trace && { trace }),
        syntaxHighlighting: run.syntaxHighlighting ?? false,
        source,
        sourceInfo: phoneSourceInfo(source),
        sourceEqualsFixture: source === workload.source,
        renderedOutputEquality: 'requires native accessibility and screenshot inspection; source equality alone is insufficient',
        revisionCount: workload.chunks.length,
        expectedDurationMs: workload.chunks.at(-1)?.dueMs ?? 0,
        deliveryDurationMs: data.deliveryDurationMs,
        totalDurationMs,
        settleMs: SETTLE_MS,
        deliveries: data.deliveries,
        rafSamples: data.rafSamples,
        commits: data.commits,
        webCommits: data.webCommits,
        errors: data.errors,
        pipeline: run.candidate === 'downstream-default' ? 'public createMarkdownStream default incremental parse / canonical Android paragraph layout' : run.candidate === 'downstream-full' ? 'internal full-parse comparison stream / canonical Android paragraph layout' : run.candidate === 'downstream-incremental' ? 'native stream incremental parse / incremental Android paragraph layout' : run.candidate === 'marked' ? 'shipping Markdown component with default FlatList virtualization' : run.candidate === 'web-streamdown' ? 'Streamdown React DOM inside Android WebView; separate platform category' : 'shipping renderer with cumulative markdown prop',
        measurementNotes: 'rAF gaps and React commits measure JS responsiveness; neither proves native presentation latency',
      });
    }
    void execute().catch(async (error) => {
      reportError(error);
      measuring = false;
      cancelAnimationFrame(frame);
      if (disposed) return;
      setStatus(`Benchmark failed: ${String(error)}`);
      if (activeConfig) {
        try {
          await post(activeConfig.endpoint ?? HOST, '/result', {
            ...activeConfig, diagnostic: Boolean(activeConfig.trace || activeConfig.diagnostic),
            ...(trace && { trace }), failed: true, errors: samples.current.errors,
          });
        } catch { /* Host logs and the visible failure retain the error. */ }
      }
    });
    return () => {
      disposed = true;
      measuring = false;
      cancelAnimationFrame(frame);
      timers.forEach(clearTimeout);
      stopTrace(false);
      stopTraceOnError.current = null;
      controller.current?.dispose();
      controller.current = null;
    };
  }, []);

  return (
    <View style={styles.root} testID="phone-benchmark-root">
      <StatusBar hidden />
      {config ? (
        <RendererBoundary onError={(error) => { samples.current.errors.push(error); stopTraceOnError.current?.(); }}>
          <Candidate config={config} snapshot={snapshot} controller={controller.current}
            onWebReady={() => webResolve.current?.()}
            onWebMessage={(message) => samples.current.webCommits.push({ ...message, receivedAtMs: samples.current.startAt ? now() - samples.current.startAt : null })}
            onError={(error) => { samples.current.errors.push(error); stopTraceOnError.current?.(); }} />
        </RendererBoundary>
      ) : <Fragment><Text style={textStyle}>{status}</Text></Fragment>}
    </View>
  );
}

const styles = StyleSheet.create({
  root: { flex: 1, backgroundColor: '#ffffff' },
  viewport: { flex: 1, backgroundColor: '#ffffff' },
  document: { padding: 16 },
  web: { flex: 1, backgroundColor: '#ffffff' },
});
