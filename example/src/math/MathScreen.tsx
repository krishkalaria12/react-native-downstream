import { useCallback, useEffect, useState } from 'react';
import { ScrollView, StyleSheet, Text, View } from 'react-native';
import {
  createMarkdownStream,
  DownstreamMarkdown,
  mathAvailable,
  type MarkdownImageEvent,
  type MarkdownMathErrorEvent,
  type MarkdownStreamController,
} from 'react-native-downstream';

import { Badge, ChipRow, Group } from '../ui/kit';
import { Hero, Screen, SectionLabel, useScreenScroll } from '../ui/screen';
import { radius, space, useTheme } from '../ui/theme';

/**
 * Every math form the engine recognizes, a formula it has to refuse, and an image
 * in each state: one that loads, one that 404s, and one the prefix list blocks.
 */
const SOURCE =
  String.raw`# Math

A display formula on its own lines:

$$
x = \frac{-b \pm \sqrt{b^2 - 4ac}}{2a}
$$

The same formula with the bracket delimiters models write:

\[
\int_0^\infty e^{-x^2}\,dx = \frac{\sqrt{\pi}}{2}
\]

A fenced block:

` +
  '```math' +
  String.raw`
\begin{pmatrix} a & b \\ c & d \end{pmatrix}
\begin{pmatrix} x \\ y \end{pmatrix}
=
\begin{pmatrix} ax + by \\ cx + dy \end{pmatrix}
` +
  '```' +
  String.raw`

A lone display formula on one line becomes a block too:

$$\sum_{k=1}^{n} k = \frac{n(n+1)}{2}$$

Colors, a brace, and a CJK character: $$\color{#e5484d}{\alpha} + \overbrace{a+b}^{2} + \text{中}$$

Inline formulas stay in the sentence, like $$E = mc^2$$ and \(a^2 + b^2 = c^2\).

A formula RaTeX refuses keeps its source:

$$
\frac{1}{
$$

# Images

A picture that loads:

![A mountain lake](https://picsum.photos/seed/downstream/800/450)

A picture that 404s:

![A missing chart](https://picsum.photos/does-not-exist.png)

A picture the prefix list blocks:

![Tracking pixel](https://tracker.example.com/pixel.png)
`;

/** Appended when held mid-stream: a formula and an image still being written. */
const OPEN_TAIL = String.raw`

$$
\lim_{x \to 0} \frac{\sin x}{x} = \frac{1}{`;

const MATH = [
  { value: 'default', label: 'Math: default' },
  { value: 'on', label: 'On' },
  { value: 'off', label: 'Off' },
] as const;

const IMAGES = [
  { value: 'on', label: 'Images: on' },
  { value: 'off', label: 'Off' },
] as const;

const STATES = [
  { value: 'finished', label: 'Finished' },
  { value: 'open', label: 'Held mid-stream' },
] as const;

type MathMode = (typeof MATH)[number]['value'];
type ImageMode = (typeof IMAGES)[number]['value'];
type StreamState = (typeof STATES)[number]['value'];

const ALLOWED = ['https://picsum.photos', 'https://fastly.picsum.photos'];
const LOG_LIMIT = 6;

function useHeldStream(state: StreamState): MarkdownStreamController | null {
  const [stream, setStream] = useState<MarkdownStreamController | null>(null);
  useEffect(() => {
    if (state !== 'open') {
      setStream(null);
      return undefined;
    }
    const controller = createMarkdownStream({ math: true });
    controller.append(SOURCE + OPEN_TAIL);
    setStream(controller);
    return () => controller.dispose();
  }, [state]);
  return stream;
}

export function MathScreen() {
  const t = useTheme();
  const scroll = useScreenScroll();
  const [math, setMath] = useState<MathMode>('default');
  const [images, setImages] = useState<ImageMode>('on');
  const [state, setState] = useState<StreamState>('finished');
  const [log, setLog] = useState<readonly string[]>([]);
  const stream = useHeldStream(state);
  const engine = mathAvailable();

  const push = useCallback((line: string) => {
    setLog((current) => [line, ...current].slice(0, LOG_LIMIT));
  }, []);
  const onMathError = useCallback(
    (event: MarkdownMathErrorEvent) =>
      push(`math error: ${event.source.trim()} — ${event.message}`),
    [push]
  );
  const onImagePress = useCallback(
    (event: MarkdownImageEvent) => push(`image press: ${event.alt}`),
    [push]
  );

  const mathProp = math === 'default' ? undefined : math === 'on';
  const shared = {
    math: mathProp,
    images: images === 'on',
    allowedImagePrefixes: ALLOWED,
    onMathError,
    onImagePress,
  };

  return (
    <Screen title="Math & images" scroll={scroll}>
      <ScrollView
        onScroll={scroll.onScroll}
        scrollEventThrottle={16}
        contentContainerStyle={styles.content}
      >
        <Hero
          title="Math & images"
          summary="Formulas typeset natively by RaTeX, and pictures loaded through the platform's own image pipeline."
        />

        <SectionLabel
          trailing={
            <Badge
              label={engine ? 'RaTeX installed' : 'no engine'}
              tone={engine ? 'live' : 'neutral'}
            />
          }
        >
          Configuration
        </SectionLabel>
        <View style={styles.controls}>
          <ChipRow options={MATH} value={math} onChange={setMath} />
          <ChipRow options={IMAGES} value={images} onChange={setImages} />
          <ChipRow options={STATES} value={state} onChange={setState} />
        </View>

        <SectionLabel>Document</SectionLabel>
        <View style={[t.s.card, styles.card]}>
          {stream ? (
            <DownstreamMarkdown stream={stream} {...shared} />
          ) : (
            <DownstreamMarkdown markdown={SOURCE} {...shared} />
          )}
        </View>

        <SectionLabel>Log</SectionLabel>
        <Group title="Math errors and image presses">
          {log.length === 0 ? (
            <Text style={[t.s.caption, styles.empty]}>Nothing yet.</Text>
          ) : (
            <View style={styles.log}>
              {log.map((line, index) => (
                <Text
                  key={`${index}-${line}`}
                  numberOfLines={2}
                  style={t.s.caption}
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

const styles = StyleSheet.create({
  content: { paddingBottom: space.xxl },
  controls: { paddingBottom: space.md, gap: space.xs },
  card: {
    marginHorizontal: space.xl,
    padding: space.lg,
    borderRadius: radius.card,
  },
  log: { gap: space.xs, padding: space.lg },
  empty: { padding: space.lg },
});
