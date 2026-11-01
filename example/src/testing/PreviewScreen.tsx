import { useEffect, useState } from 'react';
import { ScrollView, StyleSheet, Text, View } from 'react-native';
import {
  Downstream,
  type MarkdownStreamController,
} from 'react-native-downstream';

import {
  createMarkdownStreamWithMode,
  type ParseMode,
} from '../../../src/createMarkdownStream';
import { ChipRow, Group, type ChipOption } from '../ui/kit';
import { DocumentPane } from '../ui/readout';
import { Hero, Screen, useBottomInset, useScreenScroll } from '../ui/screen';
import { space, useTheme } from '../ui/theme';
import { StreamdownMarkdown } from './StreamdownMarkdown';

/**
 * Half-written shapes a model leaves behind, each held open mid-stream. Every
 * case is one chunk into an unfinished stream, so what a pane shows is exactly
 * the frame a reader would see at that moment.
 */
export const PREVIEW_CASES: readonly { source: string; note: string }[] = [
  { source: 'Hello **bold wor', note: 'an open bold span' },
  { source: '**bold *ital', note: 'nested spans close together' },
  { source: '~~strike with **bold', note: 'strike around bold' },
  { source: 'Use `map` and `filt', note: 'an open code span' },
  { source: '**bold word ', note: 'a chunk that ends on a space' },
  { source: '**bold and \\*literal', note: 'an escaped star is skipped' },
  { source: '# **Heading\n', note: 'a heading whose line has ended' },
  { source: '- item one\n- **item tw', note: 'inside a list item' },
  { source: '2 * 3 and ~hello', note: 'arithmetic and a single tilde' },
  { source: 'See [the docs](https://exa', note: 'an unfinished link' },
  { source: 'Check the [documentation', note: 'link text still open' },
  { source: 'See ![diagram](https://exa', note: 'an unfinished image' },
  { source: 'see [note](which is long', note: 'prose that is not a link' },
  { source: 'Mix steps<br', note: 'a half-arrived br' },
  { source: 'Water is H<sub>2', note: 'an open subscript' },
  { source: 'a<b and 2<3', note: 'a comparison is not a tag' },
];

type Preview = 'on' | 'off';
type StreamState = 'open' | 'finished';

const PREVIEWS: ChipOption<Preview>[] = [
  { value: 'on', label: 'Preview on' },
  { value: 'off', label: 'Preview off' },
];

const MODES: ChipOption<ParseMode>[] = [
  { value: 'full', label: 'Full parse' },
  { value: 'incremental', label: 'Incremental tail' },
];

const STATES: ChipOption<StreamState>[] = [
  { value: 'open', label: 'Still streaming' },
  { value: 'finished', label: 'Finished' },
];

export interface PreviewLaunch {
  preview?: Preview;
  mode?: ParseMode;
  state?: StreamState;
  /** Index of the first case shown, so a screenshot can page through them. */
  from?: number;
}

/** One case, streamed into a fresh session with the chosen policy. */
function DownstreamCase({
  source,
  preview,
  mode,
  state,
}: {
  source: string;
  preview: Preview;
  mode: ParseMode;
  state: StreamState;
}) {
  const [stream, setStream] = useState<MarkdownStreamController | null>(null);
  useEffect(() => {
    const controller = createMarkdownStreamWithMode({}, mode);
    controller.append(source);
    if (state === 'finished') controller.finish();
    setStream(controller);
    return () => controller.dispose();
  }, [source, mode, state]);
  return stream ? (
    <Downstream stream={stream} preview={preview === 'on'} />
  ) : (
    <Downstream markdown="" />
  );
}

export function PreviewScreen({ launch }: { launch?: PreviewLaunch }) {
  const t = useTheme();
  const scroll = useScreenScroll();
  const bottom = useBottomInset();
  const [preview, setPreview] = useState<Preview>(launch?.preview ?? 'on');
  const [mode, setMode] = useState<ParseMode>(launch?.mode ?? 'full');
  const [state, setState] = useState<StreamState>(launch?.state ?? 'open');

  useEffect(() => {
    if (launch?.preview) setPreview(launch.preview);
    if (launch?.mode) setMode(launch.mode);
    if (launch?.state) setState(launch.state);
  }, [launch]);

  const cases = PREVIEW_CASES.slice(launch?.from ?? 0);
  return (
    <Screen title="Preview" scroll={scroll}>
      <ScrollView
        onScroll={scroll.onScroll}
        scrollEventThrottle={16}
        contentContainerStyle={{ paddingBottom: bottom }}
      >
        <Hero
          title="Provisional styling"
          summary="Each case is held open mid-stream. Downstream's preview on the left, Streamdown's remend on the right."
        />
        <View style={styles.controls}>
          <Group title="Downstream">
            <ChipRow
              inset
              options={PREVIEWS}
              value={preview}
              onChange={setPreview}
            />
            <ChipRow inset options={MODES} value={mode} onChange={setMode} />
            <ChipRow inset options={STATES} value={state} onChange={setState} />
          </Group>
        </View>
        {cases.map(({ source, note }) => (
          <View key={source} style={styles.case}>
            <Text style={t.s.caption}>{note}</Text>
            <Text style={[t.s.mono, styles.source]}>
              {/* A line break shows as a visible \n, so each case reads on one line. */}
              {source.replace(/\n/g, '\\n')}
            </Text>
            <View style={styles.row}>
              <DocumentPane label="Downstream" style={styles.pane}>
                <DownstreamCase
                  source={source}
                  preview={preview}
                  mode={mode}
                  state={state}
                />
              </DocumentPane>
              <DocumentPane label="Streamdown" style={styles.pane}>
                <StreamdownMarkdown markdown={source} />
              </DocumentPane>
            </View>
          </View>
        ))}
      </ScrollView>
    </Screen>
  );
}

const styles = StyleSheet.create({
  controls: { paddingHorizontal: space.xl, paddingTop: space.lg },
  case: {
    paddingHorizontal: space.xl,
    paddingTop: space.lg,
    gap: space.xs,
  },
  source: { fontSize: 12 },
  row: { flexDirection: 'row', gap: space.sm },
  pane: { flex: 1 },
});
