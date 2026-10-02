import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { ScrollView, StyleSheet, Text, View } from 'react-native';
import {
  createMarkdownStream,
  DownstreamMarkdown,
  type MarkdownAnimateOptions,
  type MarkdownErrorEvent,
  type MarkdownStreamController,
} from 'react-native-downstream';

import { Badge, Button, ButtonRow, ChipRow, Group, Notice } from '../ui/kit';
import { Hero, Screen, SectionLabel, useScreenScroll } from '../ui/screen';
import { radius, space, useTheme } from '../ui/theme';

/**
 * Three sources, chosen for what each one shows that the others do not.
 *
 * A burst is the case the timeline exists for: a long answer arriving in one
 * chunk, where a fixed stagger would queue seconds of invisible words. A drip is
 * the opposite, one small piece at a time, where each word should fade fully. And
 * the mid-stream samples are the ones that decide whether a re-render is safe: an
 * emphasis that closes, a link that resolves, a fence that grows a line.
 */
const SOURCES = {
  burst: `# Streaming animation

A burst arrives in one chunk, and the timeline compresses it rather than queueing
it. Forty words on a forty millisecond stagger would be one and a half seconds of
text you cannot read yet; inside the backlog they fade across about a third of a
second, and every one of them gets the same fade.

The caret follows the last word that has *started*, not the end of the layout, so
it never sits after a gap of words that have not appeared yet.

- A list marker fades with its first word.
- A task box does the same, and in step with it.
- A rule is one unit, and fades as one.

| Column | What it shows |
| --- | --- |
| Words | The cascade itself |
| Caret | Where the frontier is |
| Rows | One unit per row |

\`\`\`ts
const words = 'each of these fades on the same timeline';
\`\`\`

---

That is the whole of it. A frame of animation costs a lookup and a redraw, and no
parse, no layout, and no change to any cached text.`,
  drip: `# One word at a time

Each of these words arrives alone, with a gap long enough for the fade to finish,
so every one of them is seen whole rather than caught mid-cascade.

The point of a shared timeline is that a word gets the same fade however the bytes
were chunked. A stream that pauses between words and a stream that fires forty at
once should not look like two different features.`,
  midstream: `Emphasis that closes one character at a time, a link whose target
arrives after its text, and a fence that grows a line while you watch.

hello **wor
hello **world**, and [a link](https://exa
hello **world**, and [a link](https://example.com) that resolves late.

\`\`\`python
def first():
\`\`\`

\`\`\`python
def first():
    return 1
\`\`\`

None of the text above is new when its own markers close, so none of it fades
again.`,
} as const;

type SourceId = keyof typeof SOURCES;

const SOURCE_OPTIONS = [
  { value: 'burst', label: 'Burst' },
  { value: 'drip', label: 'Slow drip' },
  { value: 'midstream', label: 'Closing markers' },
] as const satisfies readonly { value: SourceId; label: string }[];

const EFFECTS = [
  { value: 'off', label: 'Off' },
  { value: 'fadeIn', label: 'fadeIn' },
  { value: 'slideUp', label: 'slideUp' },
  { value: 'blurIn', label: 'blurIn' },
] as const;

const SPLITS = [
  { value: 'word', label: 'Word' },
  { value: 'char', label: 'Char' },
] as const;

const CARETS = [
  { value: 'none', label: 'None' },
  { value: 'block', label: 'Block' },
  { value: 'circle', label: 'Circle' },
] as const;

const STAGGERS = [
  { value: '0', label: '0' },
  { value: '20', label: '20' },
  { value: '40', label: '40' },
  { value: '90', label: '90' },
] as const;

const BACKLOGS = [
  { value: '160', label: '160' },
  { value: '320', label: '320' },
  { value: '900', label: '900' },
] as const;

const EASINGS = [
  { value: 'ease', label: 'ease' },
  { value: 'linear', label: 'linear' },
  { value: 'ease-out', label: 'ease-out' },
] as const;

/**
 * How the source is cut up and how fast the pieces are delivered.
 *
 * The three shapes are the three cases the timeline's budget exists to serve, and
 * the screen is only useful if all three are one tap away.
 */
const CADENCES = [
  { value: 'drip', label: 'Drip', intervalMs: 120, size: 6 },
  { value: 'burst', label: 'Burst', intervalMs: 16, size: 512 },
  { value: 'two-bursts', label: 'Two bursts', intervalMs: 16, size: 256 },
] as const;

type Effect = (typeof EFFECTS)[number]['value'];
type Split = (typeof SPLITS)[number]['value'];
type Caret = (typeof CARETS)[number]['value'];
type Cadence = (typeof CADENCES)[number]['value'];

const LOG_LIMIT = 8;

/**
 * A stream replayed on a schedule, disposed on unmount.
 *
 * The pieces are delivered on a timer rather than in one call, because a single
 * append is one snapshot and the cascade is only visible across several. A replay
 * is a fresh controller each time, so a repeat starts from nothing rather than
 * from the previous run's end.
 */
function useReplay(source: string, cadence: Cadence) {
  const [stream, setStream] = useState<MarkdownStreamController | null>(null);
  const [running, setRunning] = useState(false);
  const timers = useRef<ReturnType<typeof setTimeout>[]>([]);
  const config =
    CADENCES.find((entry) => entry.value === cadence) ?? CADENCES[0];

  const stop = useCallback(() => {
    for (const timer of timers.current) clearTimeout(timer);
    timers.current = [];
    setRunning(false);
  }, []);

  const start = useCallback(() => {
    for (const timer of timers.current) clearTimeout(timer);
    timers.current = [];
    // A word boundary is where a piece ends, so a word is never split across two
    // deliveries the way a real stream can split one.
    const words = source.split(/(\s+)/).filter((piece) => piece.length > 0);
    const pieces: string[] = [];
    for (let index = 0; index < words.length; index += config.size * 2) {
      pieces.push(words.slice(index, index + config.size * 2).join(''));
    }
    const controller = createMarkdownStream();
    setStream(controller);
    setRunning(true);
    pieces.forEach((piece, index) => {
      const timer = setTimeout(() => {
        controller.append(piece);
        if (index === pieces.length - 1) {
          // The last delivery finishes the stream, which lets every fade run to
          // its end and removes the caret.
          controller.finish();
          setRunning(false);
        }
      }, index * config.intervalMs);
      timers.current.push(timer);
    });
  }, [config.intervalMs, config.size, source]);

  useEffect(
    () => () => {
      for (const timer of timers.current) clearTimeout(timer);
      timers.current = [];
    },
    []
  );

  return { stream, running, start, stop };
}

export function AnimationScreen() {
  const t = useTheme();
  const scroll = useScreenScroll();
  const [sourceId, setSourceId] = useState<SourceId>('burst');
  const [cadence, setCadence] = useState<Cadence>('drip');
  const [effect, setEffect] = useState<Effect>('fadeIn');
  const [split, setSplit] = useState<Split>('word');
  const [caret, setCaret] = useState<Caret>('block');
  const [stagger, setStagger] = useState('40');
  const [backlog, setBacklog] = useState('320');
  const [easing, setEasing] = useState('ease');
  const [entries, setEntries] = useState<readonly string[]>([]);

  const source = SOURCES[sourceId];
  const { stream, running, start, stop } = useReplay(source, cadence);
  const animating = effect !== 'off';

  // One object, held by value, so a re-render with the same options sends the
  // native side nothing new. That is the same rule the library applies internally,
  // and it is what stops a fade from restarting on an unrelated render.
  const animated = useMemo<MarkdownAnimateOptions | false>(
    () =>
      animating
        ? {
            animation: effect,
            sep: split,
            easing,
            stagger: Number(stagger),
            maxBacklogMs: Number(backlog),
          }
        : false,
    [animating, backlog, effect, easing, split, stagger]
  );

  const push = useCallback((line: string) => {
    setEntries((current) => [line, ...current].slice(0, LOG_LIMIT));
  }, []);

  const onStart = useCallback(() => push('animation start'), [push]);
  const onEnd = useCallback(() => push('animation end'), [push]);
  const onError = useCallback(
    (error: MarkdownErrorEvent) => push(`${error.code}: ${error.message}`),
    [push]
  );

  const reset = useCallback(() => {
    stop();
    setEntries([]);
  }, [stop]);

  return (
    <Screen title="Animation" scroll={scroll}>
      <ScrollView
        onScroll={scroll.onScroll}
        scrollEventThrottle={16}
        contentContainerStyle={styles.content}
        keyboardShouldPersistTaps="handled"
      >
        <Hero
          title="Streaming animation"
          summary="A per-word fade-in and a caret at the writing point, decided by the engine and drawn by each platform's existing draw pass. No React render, no relayout, and no change to any cached text, per frame."
        />

        <SectionLabel trailing={<Badge label="offline" tone="neutral" />}>
          Configuration
        </SectionLabel>
        <View style={styles.controls}>
          <ChipRow
            options={SOURCE_OPTIONS}
            value={sourceId}
            onChange={setSourceId}
          />
          <ChipRow
            options={CADENCES.map(({ value, label }) => ({ value, label }))}
            value={cadence}
            onChange={setCadence}
          />
          <Text style={[t.s.caption, styles.label]}>Effect</Text>
          <ChipRow options={EFFECTS} value={effect} onChange={setEffect} />
          <Text style={[t.s.caption, styles.label]}>Split</Text>
          <ChipRow options={SPLITS} value={split} onChange={setSplit} />
          <Text style={[t.s.caption, styles.label]}>Easing</Text>
          <ChipRow options={EASINGS} value={easing} onChange={setEasing} />
          <Text style={[t.s.caption, styles.label]}>Stagger, in ms</Text>
          <ChipRow options={STAGGERS} value={stagger} onChange={setStagger} />
          <Text style={[t.s.caption, styles.label]}>Max backlog, in ms</Text>
          <ChipRow options={BACKLOGS} value={backlog} onChange={setBacklog} />
          <Text style={[t.s.caption, styles.label]}>Caret</Text>
          <ChipRow options={CARETS} value={caret} onChange={setCaret} />
          <View style={styles.row}>
            <ButtonRow>
              <Button
                label={running ? 'Streaming…' : 'Replay'}
                variant={running ? 'secondary' : 'primary'}
                onPress={running ? stop : start}
                hint="Replays the source on a timer and finishes the stream at the end"
              />
              <Button
                label="Reset"
                variant="ghost"
                onPress={reset}
                hint="Stops the replay and empties the callback log"
              />
            </ButtonRow>
          </View>
        </View>

        {effect === 'blurIn' && (
          <View style={styles.notice}>
            <Notice tone="warn" title="blurIn draws as fadeIn">
              Neither platform can blur one run of glyphs cheaply inside a text
              draw pass, and a CSS keyframe name means nothing natively. It
              warns once in development and then draws the fade.
            </Notice>
          </View>
        )}
        {cadence === 'burst' && (
          <View style={styles.notice}>
            <Notice tone="accent" title="A burst, not a queue">
              One chunk this long would queue {Math.round(source.length / 6)}{' '}
              words at a {stagger} ms stagger. Inside the {backlog} ms backlog
              they are compressed, so the last word still starts within{' '}
              {backlog} ms.
            </Notice>
          </View>
        )}

        <SectionLabel>Document</SectionLabel>
        <View style={[t.s.card, styles.card]}>
          {stream ? (
            <DownstreamMarkdown
              stream={stream}
              animated={animated}
              caret={caret === 'none' ? undefined : caret}
              onAnimationStart={onStart}
              onAnimationEnd={onEnd}
              onError={onError}
            />
          ) : (
            <DownstreamMarkdown
              markdown={source}
              animated={animated}
              caret={caret === 'none' ? undefined : caret}
              onAnimationStart={onStart}
              onAnimationEnd={onEnd}
              onError={onError}
            />
          )}
        </View>

        <SectionLabel
          trailing={
            entries.length > 0 ? (
              <Badge label={`${entries.length}`} tone="live" />
            ) : undefined
          }
        >
          Log
        </SectionLabel>
        <Group title="Animation callbacks">
          {entries.length === 0 ? (
            <Text style={[t.s.caption, styles.logEmpty]}>
              Replay the source to see the transitions.
            </Text>
          ) : (
            <View style={styles.log}>
              {entries.map((entry, index) => (
                <Text
                  key={`${index}-${entry}`}
                  numberOfLines={2}
                  style={[t.s.caption, styles.logLine, index === 0 && t.s.body]}
                >
                  {entry}
                </Text>
              ))}
            </View>
          )}
        </Group>

        <View style={styles.foot} />
      </ScrollView>
    </Screen>
  );
}

const styles = StyleSheet.create({
  content: { paddingBottom: space.xxl },
  controls: { paddingBottom: space.md, gap: space.xs },
  label: { paddingHorizontal: space.xl, paddingTop: space.sm, opacity: 0.7 },
  row: { paddingHorizontal: space.xl, paddingTop: space.sm },
  notice: { paddingHorizontal: space.xl, paddingTop: space.md },
  card: {
    marginHorizontal: space.xl,
    padding: space.lg,
    borderRadius: radius.card,
  },
  log: { gap: space.xs, padding: space.lg },
  logEmpty: { padding: space.lg },
  logLine: { opacity: 0.75 },
  foot: { height: space.xxl },
});
