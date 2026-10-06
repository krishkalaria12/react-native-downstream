import { useCallback, useEffect, useMemo, useState } from 'react';
import { ScrollView, StyleSheet, Switch, Text, View } from 'react-native';
import {
  createMarkdownStream,
  DownstreamMarkdown,
  type MarkdownErrorEvent,
  type MarkdownPlugins,
  type MarkdownStreamController,
} from 'react-native-downstream';

import { Badge, ChipRow, Group, Notice } from '../ui/kit';
import { Hero, Screen, SectionLabel, useScreenScroll } from '../ui/screen';
import { space, useTheme } from '../ui/theme';
import { BrokenRenderer, ChartRenderer, LazyChart } from './chart';
import { MermaidRenderer } from './mermaid';

/**
 * Every shape the feature has to get right, in one document.
 *
 * A chart at the top, a chart in a list item with the marker still beside it, a
 * chart in a quote with the bar beside it, a fence whose renderer throws, one
 * whose renderer loads lazily, and a fence no renderer claims — which is the
 * code card the reader already has.
 */
const SOURCE = [
  '# Fence renderers',
  '',
  'A chart drawn by a React component, in place of the code card:',
  '',
  '```chart title="Revenue by month"',
  '[{"label":"Jan","value":28},{"label":"Feb","value":55},{"label":"Mar","value":43},{"label":"Apr","value":38}]',
  '```',
  '',
  'The same chart inside a list item, whose marker the native side still draws:',
  '',
  '1. First item.',
  '2. A chart in a list:',
  '',
  '   ```chart title="In a list"',
  '   [{"label":"A","value":10},{"label":"B","value":30}]',
  '   ```',
  '',
  '3. The last item.',
  '',
  'And one inside a quote, whose bar spans the component:',
  '',
  '> ```chart title="In a quote"',
  '> [{"label":"Q1","value":20},{"label":"Q2","value":45}]',
  '> ```',
  '',
  'A chart that loads lazily draws its code card until it resolves.',
  '',
  '```chart title="Lazy"',
  '[{"label":"L","value":25},{"label":"R","value":60}]',
  '```',
  '',
  'A chart whose renderer throws falls back to the code card, with copy and',
  'download, and reports once:',
  '',
  'A diagram, drawn with mermaid.js in a WebView that only shows the SVG:',
  '',
  '```mermaid',
  'flowchart TD',
  '    A[Model writes] --> B{Parses?}',
  '    B -->|Yes| C[Diagram]',
  '    B -->|No| D[The source as prose]',
  '```',
  '',
  '```broken',
  'this renderer always throws',
  '```',
  '',
  'A fence no renderer claims is the code card, exactly as before:',
  '',
  '```json',
  '{"kept":"as code"}',
  '```',
  '',
].join('\n');

/** Appended when held mid-stream: a chart fence the model has not finished. */
const OPEN_TAIL = '\n```chart title="Still writing"\n[{"label":"Q1","value":';

const MODES = [
  { value: 'controlled', label: 'Controlled' },
  { value: 'stream', label: 'Streamed' },
  { value: 'held', label: 'Held mid-stream' },
] as const;

type Mode = (typeof MODES)[number]['value'];

const LOG_LIMIT = 6;

function useHeldStream(): MarkdownStreamController | null {
  const [stream, setStream] = useState<MarkdownStreamController | null>(null);
  useEffect(() => {
    const controller = createMarkdownStream();
    controller.append(SOURCE + OPEN_TAIL);
    setStream(controller);
    return () => controller.dispose();
  }, []);
  return stream;
}

export function RenderersScreen() {
  const t = useTheme();
  const scroll = useScreenScroll();
  const [mode, setMode] = useState<Mode>('controlled');
  const [streaming, setStreaming] = useState(false);
  const [log, setLog] = useState<readonly string[]>([]);
  const held = useHeldStream();

  const push = useCallback((line: string) => {
    setLog((current) => [line, ...current].slice(0, LOG_LIMIT));
  }, []);

  const onError = useCallback(
    (error: MarkdownErrorEvent) => {
      if (error.code === 'renderer_failed') {
        push(`renderer_failed: ${error.language} — ${error.message}`);
      } else {
        push(`${error.code}: ${error.message}`);
      }
    },
    [push]
  );

  /**
   * Registered by value, so a re-render of this screen asks the engine about
   * nothing: the language list and the component identities are what the view
   * compares, exactly as it does for `controls` and `theme`.
   */
  const plugins = useMemo<MarkdownPlugins>(
    () => ({
      renderers: [
        { language: ['chart', 'lazychart'], component: ChartRenderer },
        { language: 'lazy', component: LazyChart },
        { language: 'broken', component: BrokenRenderer },
        // Phase 7's example: a `mermaid` fence becomes a diagram, drawn with
        // mermaid.js in a WebView that only shows the SVG it is given. The core
        // ships no diagram code; this screen is where it lives.
        { language: 'mermaid', component: MermaidRenderer },
      ],
    }),
    []
  );

  /** A stream that appends the document a piece at a time, so completeness matters. */
  const streamed = useStreamedSource(streaming);

  /** The held stream's source, so the controlled view can show the same text. */
  const heldSource = SOURCE + OPEN_TAIL;

  return (
    <Screen title="Renderers" scroll={scroll}>
      <ScrollView
        onScroll={scroll.onScroll}
        scrollEventThrottle={16}
        contentContainerStyle={styles.content}
      >
        <Hero
          title="Fence renderers"
          summary="Your own React components, drawn in place of a code card, measured and placed by the native view."
        />

        <SectionLabel>Configuration</SectionLabel>
        <View style={styles.controls}>
          <ChipRow options={MODES} value={mode} onChange={setMode} />
          <View style={styles.switchRow}>
            <Text style={t.s.caption}>Type the document in</Text>
            <Switch value={streaming} onValueChange={setStreaming} />
          </View>
        </View>

        <SectionLabel>Document</SectionLabel>
        <View style={[t.s.card, styles.card]}>
          {mode === 'stream' && streamed ? (
            <DownstreamMarkdown
              stream={streamed}
              plugins={plugins}
              onError={onError}
            />
          ) : mode === 'held' && held ? (
            <DownstreamMarkdown
              stream={held}
              plugins={plugins}
              onError={onError}
            />
          ) : (
            <DownstreamMarkdown
              markdown={mode === 'held' ? heldSource : SOURCE}
              plugins={plugins}
              onError={onError}
            />
          )}
        </View>

        <Notice tone="accent" title="What to look at">
          A chart is as tall as it draws, so the text below it moves when the
          chart does and never otherwise. The list marker and the quote bar are
          native, and they sit at the chart's column rather than the card's. A
          renderer that throws, and one that has not loaded, both leave the code
          card underneath — copy and download still work.
        </Notice>

        <SectionLabel trailing={<Badge label={`${log.length} events`} />}>
          Log
        </SectionLabel>
        <Group title="renderer_failed">
          {log.length === 0 ? (
            <Text style={[t.s.caption, styles.empty]}>
              Nothing yet. The broken renderer reports once its fence settles.
            </Text>
          ) : (
            <View style={styles.logBody}>
              {log.map((line, index) => (
                <Text
                  key={`${line}-${index}`}
                  style={[t.s.caption, styles.logLine]}
                >
                  {line}
                </Text>
              ))}
            </View>
          )}
        </Group>
      </ScrollView>
    </Screen>
  );
}

/**
 * A stream that appends the document a chunk at a time, so a fence's
 * `isIncomplete` and its settledness are both exercised. Returns null until the
 * first chunk has been appended, and again once the stream is disposed.
 */
function useStreamedSource(enabled: boolean): MarkdownStreamController | null {
  const [stream, setStream] = useState<MarkdownStreamController | null>(null);
  useEffect(() => {
    if (!enabled) {
      setStream(null);
      return undefined;
    }
    const controller = createMarkdownStream();
    setStream(controller);
    let index = 0;
    const chunks = SOURCE.match(/[\s\S]{1,48}/g) ?? [];
    const timer = setInterval(() => {
      const chunk = chunks[index];
      if (chunk === undefined) {
        clearInterval(timer);
        controller.finish();
        return;
      }
      index += 1;
      controller.append(chunk);
    }, 24);
    return () => {
      clearInterval(timer);
      controller.dispose();
    };
  }, [enabled]);
  return stream;
}

const styles = StyleSheet.create({
  content: {
    padding: space.lg,
    paddingBottom: space.xxxl,
    gap: space.md,
  },
  controls: {
    gap: space.sm,
  },
  switchRow: {
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
    gap: space.sm,
  },
  card: {
    padding: space.md,
  },
  empty: {
    opacity: 0.6,
    paddingHorizontal: space.lg,
    paddingBottom: space.lg,
  },
  logBody: {
    paddingHorizontal: space.lg,
    paddingBottom: space.lg,
  },
  logLine: {
    paddingVertical: 2,
  },
});
