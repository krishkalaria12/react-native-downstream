import {
  Component,
  Fragment,
  useEffect,
  useLayoutEffect,
  useRef,
  useState,
} from 'react';
import type { ReactNode } from 'react';
import { ScrollView, StatusBar, StyleSheet, Text, View } from 'react-native';
import { fetch as expoFetch } from 'expo/fetch';
import { createMarkdownStream, Downstream } from 'react-native-downstream';
import type { MarkdownStreamController } from 'react-native-downstream';
import {
  createMarkdownStreamWithMode,
  getStreamState,
} from '../../../src/createMarkdownStream';
import {
  setStreamTracing,
  subscribeStreamTrace,
} from '../../../src/streamTrace';
import type { StreamTraceEvent } from '../../../src/streamTrace';
import { StreamdownText } from 'react-native-streamdown';
import { EnrichedMarkdownText } from 'react-native-enriched-markdown';
import MarkdownDisplay from 'react-native-markdown-display';
import Marked from 'react-native-marked';
import MarkdownRenderer from 'react-native-markdown-renderer';
import {
  phoneFixedChunks,
  phoneSourceInfo,
  phoneWorkloads,
} from './phoneWorkloads';
import type { PhoneChunk, PhoneWorkload } from './phoneWorkloads';
import { replayFinishes } from './replayFinish';

const mathOptions = { singleDollarTextMath: true };

const downstreamTheme = {
  colors: {
    foreground: '#111111',
    mutedForeground: '#555555',
    primary: '#1565c0',
    muted: '#f4f4f4',
    border: '#aaaaaa',
  },
  maxFontSizeMultiplier: 1,
};
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
const markedTheme = {
  colors: {
    text: '#111111',
    code: '#f4f4f4',
    link: '#1565c0',
    border: '#aaaaaa',
  },
};
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
  codeBlock: {
    ...textStyle,
    fontFamily: 'monospace',
    backgroundColor: '#f4f4f4',
  },
};

export type PhoneCandidate =
  | 'downstream-default'
  | 'downstream-full'
  | 'downstream-incremental'
  | 'streamdown'
  | 'enriched'
  | 'markdown-display'
  | 'marked'
  | 'markdown-renderer';

const SHARED_PIPELINE = 'shipping renderer with cumulative markdown prop';
/** What each candidate does per update, recorded with every result. */
const PIPELINES: Record<PhoneCandidate, string> = {
  'downstream-default':
    'public createMarkdownStream default incremental parse / canonical Android paragraph layout',
  'downstream-full':
    'internal full-parse comparison stream / canonical Android paragraph layout',
  'downstream-incremental':
    'native stream incremental parse / incremental Android paragraph layout',
  'marked': 'shipping Markdown component with default FlatList virtualization',
  'streamdown': SHARED_PIPELINE,
  'enriched': SHARED_PIPELINE,
  'markdown-display': SHARED_PIPELINE,
  'markdown-renderer': SHARED_PIPELINE,
};

interface RunConfig {
  candidate: PhoneCandidate;
  workload: string;
  runId: string;
  mode: 'stream' | 'static' | 'correctness';
  warmup?: boolean;
  trace?: boolean;
  diagnostic?: boolean;
  syntaxHighlighting?: boolean;
  animated?: boolean;
  settleMs?: number;
  finishDelayMs?: number;
  math?: boolean;
  endpoint?: string;
  source?: string;
  chunks?: PhoneChunk[] | string[];
  dueMs?: number[];
  /** Live runs stream model deltas relayed by the host as they arrive. */
  live?: { threads: number };
}
/** One line of the host's live NDJSON stream. */
type LiveMessage =
  | { thread: number; text: string }
  | { thread: number; finished: true }
  | { done: true }
  | { error: string };
/** Every concurrent view's text; single-stream workloads have one thread. */
interface Snapshot {
  sources: string[];
  revision: number;
  finished: boolean;
}
interface RunSamples {
  deliveries: {
    revision: number;
    /** Scheduled time in replay runs; live deltas have no schedule. */
    dueMs: number | null;
    actualMs: number;
    wallTimeMs: number;
    latenessMs: number | null;
    codepoints: number;
    thread: number;
  }[];
  rafSamples: { atMs: number; gapMs: number }[];
  commits: { revision: number; atMs: number }[];
  errors: string[];
  startAt: number;
  startWallTimeMs: number;
  finishes: { thread: number; actualMs: number; wallTimeMs: number }[];
  streamDone: { actualMs: number; wallTimeMs: number } | null;
  deliveryDurationMs: number;
}
interface TraceSamples {
  enabled: true;
  sessionId: string;
  sessions: { sessionId: string; thread: number }[];
  eventLimit: number;
  droppedEvents: number;
  intervalComplete: boolean;
  events: StreamTraceEvent[];
}
const now = () =>
  (globalThis as { performance?: { now: () => number } }).performance?.now() ??
  Date.now();

/**
 * The host escapes every non-ASCII character in its stream, so each byte is one
 * character and a network read can split anywhere without breaking a character.
 */
function asciiText(bytes: Uint8Array) {
  let text = '';
  for (let index = 0; index < bytes.length; index += 1) {
    text += String.fromCharCode(bytes[index]!);
  }
  return text;
}

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
    if (
      !Number.isFinite(chunk.dueMs) ||
      chunk.dueMs < 0 ||
      chunk.dueMs < previousDue
    ) {
      throw new Error(
        'Chunk due times must be finite, nonnegative and monotonic'
      );
    }
    previousDue = chunk.dueMs;
  }
  if (config.mode !== 'stream') {
    return {
      source: workload.source,
      chunks: [{ text: workload.source, dueMs: 0 }],
    };
  }
  return workload;
}

class RendererBoundary extends Component<
  {
    children: ReactNode;
    onError: (error: string) => void;
  },
  { failed: boolean }
> {
  state = { failed: false };
  static getDerivedStateFromError() {
    return { failed: true };
  }
  componentDidCatch(error: Error) {
    this.props.onError(String(error));
  }
  render() {
    return this.state.failed ? (
      <Text accessibilityLabel="renderer-error">Renderer failed</Text>
    ) : (
      this.props.children
    );
  }
}

function Candidate({
  config,
  source,
  controller,
  testID,
  onError,
}: {
  config: RunConfig;
  source: string;
  controller: MarkdownStreamController | null;
  testID: string;
  onError: (error: string) => void;
}) {
  const syntaxHighlighting = config.syntaxHighlighting ?? false;
  const markdownStyle = syntaxHighlighting
    ? coloredNativeMarkdownStyle
    : nativeMarkdownStyle;
  if (config.candidate === 'marked') {
    return (
      <Marked
        value={source}
        styles={markedStyle}
        theme={markedTheme}
        selectable={false}
        flatListProps={{
          testID,
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
        <Downstream
          stream={controller}
          androidParagraphLayout={
            config.candidate === 'downstream-incremental'
              ? 'incremental'
              : 'canonical'
          }
          markdownStyle={markdownStyle}
          theme={downstreamTheme}
          animated={config.animated ?? false}
          syntaxHighlighting={syntaxHighlighting}
          codeBlock={{ lineNumbers: false, maxHeight: 0 }}
          preview={false}
          controls={false}
          math={config.math ? mathOptions : false}
          selectable={false}
          onError={(error) => onError(JSON.stringify(error))}
        />
      ) : null;
      break;
    case 'streamdown':
      document = (
        <StreamdownText
          markdown={source}
          flavor="github"
          markdownStyle={markdownStyle}
          streamingAnimation={config.animated ?? false}
          selectable={false}
          allowFontScaling={false}
          md4cFlags={{ latexMath: config.math ?? false }}
        />
      );
      break;
    case 'enriched':
      document = (
        <EnrichedMarkdownText
          markdown={source}
          flavor="github"
          markdownStyle={markdownStyle}
          streamingAnimation={config.animated ?? false}
          selectable={false}
          allowFontScaling={false}
          md4cFlags={{ latexMath: config.math ?? false }}
        />
      );
      break;
    case 'markdown-display':
      document = (
        <MarkdownDisplay style={jsMarkdownStyle}>{source}</MarkdownDisplay>
      );
      break;
    case 'markdown-renderer':
      document = (
        <MarkdownRenderer style={rendererStyle} maxTopLevelChildren={null}>
          {source}
        </MarkdownRenderer>
      );
      break;
    default:
      throw new Error(`Unsupported candidate: ${String(config.candidate)}`);
  }
  return (
    <ScrollView
      testID={testID}
      style={styles.viewport}
      contentContainerStyle={styles.document}
    >
      {document}
    </ScrollView>
  );
}

/** One process launch runs one sample; final output stays mounted for native capture. */
export default function PhoneBenchmark() {
  const [config, setConfig] = useState<RunConfig | null>(null);
  const [snapshot, setSnapshot] = useState<Snapshot>({
    sources: [''],
    revision: 0,
    finished: false,
  });
  const [status, setStatus] = useState('Waiting for benchmark configuration');
  const controllers = useRef<MarkdownStreamController[]>([]);
  const samples = useRef<RunSamples>({
    deliveries: [],
    rafSamples: [],
    commits: [],
    errors: [],
    startAt: 0,
    startWallTimeMs: 0,
    finishes: [],
    streamDone: null,
    deliveryDurationMs: 0,
  });
  const stopTraceOnError = useRef<(() => void) | null>(null);

  useLayoutEffect(() => {
    if (snapshot.revision > 0 && samples.current.startAt > 0) {
      samples.current.commits.push({
        revision: snapshot.revision,
        atMs: now() - samples.current.startAt,
      });
    }
  }, [snapshot]);

  useEffect(() => {
    let disposed = false;
    let measuring = false;
    let frame = 0;
    const timers = new Set<ReturnType<typeof setTimeout>>();
    const delay = (ms: number) =>
      new Promise<void>((resolve) => {
        const timer = setTimeout(() => {
          timers.delete(timer);
          resolve();
        }, ms);
        timers.add(timer);
      });
    let activeConfig: RunConfig | null = null;
    let trace: TraceSamples | undefined;
    let traceWriteIndex = 0;
    let unsubscribeTrace: (() => void) | undefined;
    const stopTrace = (intervalComplete: boolean) => {
      if (!unsubscribeTrace) return;
      if (trace) trace.intervalComplete = intervalComplete;
      for (const controller of controllers.current) {
        try {
          setStreamTracing(controller, false);
        } catch (error) {
          if (trace) trace.intervalComplete = false;
          samples.current.errors.push(`Trace cleanup: ${String(error)}`);
        }
      }
      unsubscribeTrace();
      unsubscribeTrace = undefined;
      if (trace && trace.droppedEvents > 0) {
        trace.events = [
          ...trace.events.slice(traceWriteIndex),
          ...trace.events.slice(0, traceWriteIndex),
        ];
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
          if (next?.runId) {
            activeConfig = next;
            break;
          }
        } catch (error) {
          setStatus(`Waiting: ${String(error)}`);
        }
        await delay(500);
      }
      if (disposed || !activeConfig) return;
      const run = activeConfig;
      const finishDelayMs = run.finishDelayMs ?? 0;
      if (
        !Number.isFinite(finishDelayMs) ||
        finishDelayMs < 0 ||
        finishDelayMs > 2147483647 ||
        (run.live && finishDelayMs > 0)
      ) {
        throw new Error(
          'Finish delay must be a valid replay-only nonnegative timer duration'
        );
      }
      const settleMs = run.settleMs ?? SETTLE_MS;
      if (!Number.isFinite(settleMs) || settleMs <= 0) {
        throw new Error('Settle budget must be a positive finite number');
      }
      if (
        run.animated &&
        !run.candidate.startsWith('downstream-') &&
        run.candidate !== 'streamdown' &&
        run.candidate !== 'enriched'
      ) {
        throw new Error(
          `Streaming animations are unsupported for ${run.candidate}`
        );
      }
      const endpoint = run.endpoint ?? HOST;
      const workload = run.live ? null : await getWorkload(run);
      if (disposed) return;
      // Concurrent workloads tag each chunk with the view that receives it.
      const threadCount = run.live
        ? run.live.threads
        : 1 +
          Math.max(0, ...workload!.chunks.map((chunk) => chunk.thread ?? 0));
      if (run.trace && !run.candidate.startsWith('downstream-')) {
        throw new Error('Diagnostic tracing requires a Downstream candidate');
      }
      if (run.candidate.startsWith('downstream-')) {
        const options = {
          math: run.math ? mathOptions : false,
          onError: (error: unknown) => reportError(JSON.stringify(error)),
        };
        controllers.current = Array.from({ length: threadCount }, () =>
          run.candidate === 'downstream-default'
            ? createMarkdownStream({ onError: options.onError })
            : createMarkdownStreamWithMode(
                options,
                run.candidate === 'downstream-full' ? 'full' : 'incremental'
              )
        );
        if (run.trace) {
          const sessions = controllers.current.map((controller, thread) => ({
            sessionId: getStreamState(controller).sessionId,
            thread,
          }));
          const sessionIds = new Set(
            sessions.map((session) => session.sessionId)
          );
          const collection: TraceSamples = {
            enabled: true,
            sessionId: sessions[0]!.sessionId,
            sessions,
            eventLimit: TRACE_EVENT_LIMIT,
            droppedEvents: 0,
            intervalComplete: false,
            events: [],
          };
          trace = collection;
          // Trace delivery stays in memory; one result POST occurs after the interval.
          unsubscribeTrace = subscribeStreamTrace((entry) => {
            if (!sessionIds.has(entry.sessionId)) return;
            if (collection.events.length < collection.eventLimit)
              collection.events.push(entry);
            else {
              // Keep the tail of huge runs, with explicit truncation metadata.
              collection.events[traceWriteIndex] = entry;
              traceWriteIndex = (traceWriteIndex + 1) % collection.eventLimit;
              collection.droppedEvents += 1;
            }
          });
          controllers.current.forEach((controller) =>
            setStreamTracing(controller, true)
          );
        }
      }
      setSnapshot({
        sources: Array<string>(threadCount).fill(''),
        revision: 0,
        finished: false,
      });
      setConfig(run);
      const event = (type: string, extra = {}) =>
        post(endpoint, '/event', {
          runId: run.runId,
          candidate: run.candidate,
          workload: run.workload,
          mode: run.mode,
          type,
          appTimeMs: now(),
          wallTimeMs: Date.now(),
          ...extra,
        });
      await event('mounted');
      // Common empty-renderer mount settling. Startup is outside the measured run.
      await delay(1000);
      if (disposed) return;
      const data = samples.current;
      let previousFrame = 0;
      const observeFrame = () => {
        if (!measuring || disposed) return;
        const at = now();
        data.rafSamples.push({
          atMs: at - data.startAt,
          gapMs: at - previousFrame,
        });
        previousFrame = at;
        frame = requestAnimationFrame(observeFrame);
      };
      const beginMeasuring = () => {
        data.startAt = now();
        data.startWallTimeMs = Date.now();
        previousFrame = data.startAt;
        measuring = true;
        frame = requestAnimationFrame(observeFrame);
      };
      let source = '';
      const sources = Array<string>(threadCount).fill('');
      let revision = 0;
      const deliver = (thread: number, text: string, dueMs: number | null) => {
        const actualMs = now() - data.startAt;
        const wallTimeMs = Date.now();
        source += text;
        sources[thread] += text;
        revision += 1;
        data.deliveries.push({
          revision,
          dueMs,
          actualMs,
          wallTimeMs,
          latenessMs: dueMs == null ? null : actualMs - dueMs,
          codepoints: Array.from(text).length,
          thread,
        });
        controllers.current[thread]?.append(text);
      };
      const finishThread = (thread: number) => {
        if (disposed) return;
        data.finishes.push({
          thread,
          actualMs: now() - data.startAt,
          wallTimeMs: Date.now(),
        });
        controllers.current[thread]?.finish();
      };

      if (run.live) {
        // The host relays each model delta the moment it arrives; the window
        // starts with the first one, so model thinking time is not measured.
        const response = await expoFetch(
          `${endpoint}/stream?runId=${encodeURIComponent(run.runId)}`
        );
        if (!response.ok || !response.body) {
          throw new Error(`Live stream: HTTP ${response.status}`);
        }
        const reader = response.body.getReader();
        let pending = '';
        let done = false;
        while (!done && !disposed) {
          const read = await reader.read();
          if (read.done) break;
          pending += asciiText(read.value);
          let newline = pending.indexOf('\n');
          while (newline >= 0) {
            const message = JSON.parse(
              pending.slice(0, newline)
            ) as LiveMessage;
            pending = pending.slice(newline + 1);
            newline = pending.indexOf('\n');
            if ('error' in message)
              throw new Error(`Live model: ${message.error}`);
            if ('done' in message) {
              data.streamDone = {
                actualMs: now() - data.startAt,
                wallTimeMs: Date.now(),
              };
              done = true;
              break;
            }
            if ('finished' in message) {
              finishThread(message.thread);
              continue;
            }
            if (!measuring) {
              beginMeasuring();
              void event('start').catch(reportError);
            }
            deliver(message.thread, message.text, null);
          }
          setSnapshot({ sources: [...sources], revision, finished: done });
        }
        if (disposed) return;
        if (!done)
          throw new Error('Live stream ended before the host finished');
        if (!measuring) throw new Error('Live stream delivered no text');
      } else {
        const chunks = workload!.chunks;
        const finishes = replayFinishes(finishDelayMs, delay, finishThread);
        const lastChunk = new Map<number, number>();
        chunks.forEach((chunk, index) =>
          lastChunk.set(chunk.thread ?? 0, index)
        );
        await event('start');
        beginMeasuring();
        for (let index = 0; index < chunks.length; index += 1) {
          const chunk = chunks[index]!;
          const thread = chunk.thread ?? 0;
          const wait = chunk.dueMs - (now() - data.startAt);
          if (wait > 0) await delay(wait);
          if (disposed) return;
          deliver(thread, chunk.text, chunk.dueMs);
          if (lastChunk.get(thread) === index) finishes.schedule(thread);
          setSnapshot({
            sources: [...sources],
            revision,
            finished: index === chunks.length - 1,
          });
        }
        if (chunks.length === 0) {
          for (let thread = 0; thread < threadCount; thread += 1) {
            finishes.schedule(thread);
          }
          setSnapshot({ sources, revision: 0, finished: true });
        }
        await finishes.done();
        if (disposed) return;
        data.streamDone = {
          actualMs: now() - data.startAt,
          wallTimeMs: Date.now(),
        };
      }
      data.deliveryDurationMs = now() - data.startAt;
      // All renderers get the same settle budget; native readiness is not a speed metric.
      void event('delivery-end').catch(reportError);
      await delay(settleMs);
      const totalDurationMs = now() - data.startAt;
      measuring = false;
      cancelAnimationFrame(frame);
      stopTrace(data.errors.length === 0);
      await event('end', { totalDurationMs });
      if (disposed) return;
      await post(endpoint, '/result', {
        schemaVersion: 1,
        ...run,
        diagnostic: Boolean(run.trace || run.diagnostic),
        ...(trace && { trace }),
        syntaxHighlighting: run.syntaxHighlighting ?? false,
        animationEnabled: run.animated ?? false,
        finishDelayAppliedMs: finishDelayMs,
        source,
        sourceInfo: phoneSourceInfo(source),
        sourceEqualsFixture: workload ? source === workload.source : null,
        threadCount,
        threadSources: sources.map(phoneSourceInfo),
        // Live text differs per run, so the host checks it against what it relayed.
        ...(run.live && { threadTexts: sources }),
        renderedOutputEquality:
          'requires native accessibility and screenshot inspection; source equality alone is insufficient',
        revisionCount: revision,
        expectedDurationMs: workload?.chunks.at(-1)?.dueMs ?? null,
        deliveryDurationMs: data.deliveryDurationMs,
        startWallTimeMs: data.startWallTimeMs,
        finishes: data.finishes,
        streamDone: data.streamDone,
        totalDurationMs,
        settleMs,
        deliveries: data.deliveries,
        rafSamples: data.rafSamples,
        commits: data.commits,
        errors: data.errors,
        pipeline: PIPELINES[run.candidate],
        measurementNotes:
          'rAF gaps and React commits measure JS responsiveness; neither proves native presentation latency',
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
            ...activeConfig,
            diagnostic: Boolean(activeConfig.trace || activeConfig.diagnostic),
            ...(trace && { trace }),
            failed: true,
            errors: samples.current.errors,
          });
        } catch {
          /* Host logs and the visible failure retain the error. */
        }
      }
    });
    return () => {
      disposed = true;
      measuring = false;
      cancelAnimationFrame(frame);
      timers.forEach(clearTimeout);
      stopTrace(false);
      stopTraceOnError.current = null;
      controllers.current.forEach((controller) => controller.dispose());
      controllers.current = [];
    };
  }, []);

  return (
    <View style={styles.root} testID="phone-benchmark-root">
      <StatusBar hidden />
      {config ? (
        <RendererBoundary
          onError={(error) => {
            samples.current.errors.push(error);
            stopTraceOnError.current?.();
          }}
        >
          {/* Concurrent streams share the screen, so every view lays out and draws. */}
          {snapshot.sources.map((source, thread) => (
            <View
              key={thread}
              style={thread === 0 ? styles.pane : [styles.pane, styles.divider]}
            >
              <Candidate
                config={config}
                source={source}
                controller={controllers.current[thread] ?? null}
                testID={
                  thread === 0
                    ? 'phone-benchmark-document'
                    : `phone-benchmark-document-${thread}`
                }
                onError={(error) => {
                  samples.current.errors.push(error);
                  stopTraceOnError.current?.();
                }}
              />
            </View>
          ))}
        </RendererBoundary>
      ) : (
        <Fragment>
          <Text style={textStyle}>{status}</Text>
        </Fragment>
      )}
    </View>
  );
}

const styles = StyleSheet.create({
  root: { flex: 1, backgroundColor: '#ffffff' },
  viewport: { flex: 1, backgroundColor: '#ffffff' },
  document: { padding: 16 },
  pane: { flex: 1 },
  divider: {
    borderTopWidth: StyleSheet.hairlineWidth,
    borderTopColor: '#aaaaaa',
  },
});
