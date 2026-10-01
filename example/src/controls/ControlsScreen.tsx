import { useCallback, useEffect, useState } from 'react';
import { ScrollView, StyleSheet, Text, View } from 'react-native';
import {
  createMarkdownStream,
  DownstreamMarkdown,
  type MarkdownControlEvent,
  type MarkdownControls,
  type MarkdownErrorEvent,
  type MarkdownStreamController,
} from 'react-native-downstream';

import { Badge, Button, ButtonRow, ChipRow, Group, Notice } from '../ui/kit';
import { Hero, Screen, SectionLabel, useScreenScroll } from '../ui/screen';
import { radius, space, useTheme } from '../ui/theme';

/**
 * A document with one of everything a control can act on: a short fence, a long
 * fence that needs wrapping, an indented block with no language, and a table
 * with an alignment row.
 */
const SOURCE = `# Line numbers, titles, and a capped height

Numbers are on by default and every one of these headers is drawn natively. A
fence's own meta decides its title and its first line number; the cap decides how
tall a body grows before it scrolls inside its card.

## A titled block numbered from 10

\`\`\`ts title="src/app/load.ts" startLine=10
export async function load(id: string) {
  const res = await fetch(\`/api/\${id}\`);
  return res.json();
}
\`\`\`

## One that hides its numbers

\`\`\`python noLineNumbers
    for row in rows:
        total += row.price * row.qty
    return total
\`\`\`

## Crossing from 99 to 100

\`\`\`ts startLine=98
  98
  99
 100
 101
 102
\`\`\`

## A long path, truncated in the middle

\`\`\`ts title="packages/react-native-downstream/src/android/main/java/com/downstream/RenderedDocument.kt"
const x = 1;
\`\`\`

## Every other spelling of a title

The Docusaurus, rehype-pretty code, Expressive Code, and Nextra spellings all
name the same block, and \`lang:path\` is what a model writes when it means a
file name.

\`\`\`ts filename="named-by-filename.ts"
const a = 1;
\`\`\`

\`\`\`ts showLineNumbers{42}
def third(): ...
\`\`\`

\`\`\`rust:src/lib.rs
pub fn main() {}
\`\`\`

## A block past the cap, with an expand button

\`\`\`python
def total(rows):
    result = 0
    for row in rows:
        price = row.price
        quantity = row.quantity
        if quantity > 100:
            price = price * 0.9
        result += price * quantity
    return result


def average(rows):
    if not rows:
        return 0
    return total(rows) / len(rows)


def report(rows, limit=100):
    for row in rows:
        if row.price > limit:
            print(row.name, row.price, row.quantity)
        elif row.quantity > 10:
            print(row.name, "bulk", row.quantity)
        else:
            print(row.name, row.price)
    return average(rows)
\`\`\`

## An indented block, which has no language

    no info string, so the download keeps the source extension

## A table

| Control | Format | Default label |
| --- | --- | --- |
| Code copy | text | Copy Code |
| Code download | source file | Download file |
| Table copy | markdown, csv, tsv | Copy table |
| Table download | csv, markdown | Download table |
`;

/**
 * Appended when the document is held mid-stream: a fence that never closes, so
 * its block is still being written, its controls stay off, and a capped card
 * follows the newest line that has arrived.
 */
const OPEN_TAIL = `
## A block still being written

\`\`\`python title="still-arriving.py" startLine=1
def pending(rows):
    for row in rows:
        if row.price > 100:
            yield row
`;

const STATES = [
  { value: 'finished', label: 'Finished' },
  { value: 'open', label: 'Held mid-stream' },
] as const;

type StreamState = (typeof STATES)[number]['value'];

/** A stream holding the document open, or null when it is shown finished. */
function useHeldStream(state: StreamState): MarkdownStreamController | null {
  const [stream, setStream] = useState<MarkdownStreamController | null>(null);
  useEffect(() => {
    if (state !== 'open') {
      setStream(null);
      return undefined;
    }
    const controller = createMarkdownStream();
    controller.append(SOURCE + OPEN_TAIL);
    setStream(controller);
    return () => controller.dispose();
  }, [state]);
  return stream;
}

const MODES = [
  { value: 'defaults', label: 'Defaults' },
  { value: 'titled', label: 'Custom names' },
  { value: 'off', label: 'Controls off' },
  { value: 'no-expand', label: 'No expand' },
  { value: 'no-numbers', label: 'No numbers' },
  { value: 'uncapped', label: 'Uncapped' },
] as const;

type Mode = (typeof MODES)[number]['value'];

const SEPARATORS = [
  { value: 'auto', label: 'Auto' },
  { value: ',', label: 'Comma' },
  { value: ';', label: 'Semicolon' },
  { value: '\t', label: 'Tab' },
] as const;

type Separator = (typeof SEPARATORS)[number]['value'];

const LOG_LIMIT = 6;

function describe(event: MarkdownControlEvent): string {
  if (event.action === 'wrap') {
    return `${event.block} wrap ${event.wrapped === true ? 'on' : 'off'}`;
  }
  if (event.action === 'expand') {
    return `${event.block} expand ${event.expanded === true ? 'on' : 'off'}`;
  }
  const format = event.format === undefined ? '' : ` as ${event.format}`;
  return `${event.block} ${event.action}${format}`;
}

function note(event: MarkdownControlEvent): string {
  if (event.action === 'download') {
    return `${describe(event)} → ${event.filename ?? 'no file name'}`;
  }
  if (event.action === 'copy' && event.block === 'table') {
    return `${describe(event)}`;
  }
  return describe(event);
}

function useLog() {
  const [entries, setEntries] = useState<readonly string[]>([]);
  const push = useCallback((line: string) => {
    setEntries((current) => [line, ...current].slice(0, LOG_LIMIT));
  }, []);
  const clear = useCallback(() => setEntries([]), []);
  return { entries, push, clear };
}

/** The most recent presses, newest first. */
function Log({ entries }: { entries: readonly string[] }) {
  const t = useTheme();
  if (entries.length === 0) {
    return (
      <Text style={[t.s.caption, styles.logEmpty]}>
        Press a control to see what it reports.
      </Text>
    );
  }
  return (
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
  );
}

export function ControlsScreen() {
  const t = useTheme();
  const scroll = useScreenScroll();
  const [mode, setMode] = useState<Mode>('defaults');
  const [separator, setSeparator] = useState<Separator>('auto');
  const [wrap, setWrap] = useState(false);
  const [numbers, setNumbers] = useState(true);
  const [maxHeight, setMaxHeight] = useState<number | 'none'>(400);
  const [streamState, setStreamState] = useState<StreamState>('finished');
  const stream = useHeldStream(streamState);
  const { entries, push, clear } = useLog();

  const onError = useCallback(
    (error: MarkdownErrorEvent) => push(`${error.code}: ${error.message}`),
    [push]
  );

  const record = useCallback(
    (event: MarkdownControlEvent) => push(note(event)),
    [push]
  );

  const controls = useCallback((): MarkdownControls => {
    if (mode === 'off') return false;
    if (mode === 'no-expand') return { code: { expand: false } };
    if (mode === 'titled') {
      return {
        code: {
          copy: { onCopy: record },
          download: { filename: 'snippet', onDownload: record },
        },
        table: {
          copy: { onCopy: record },
          download: { filename: 'rows', onDownload: record },
          csvSeparator: separator,
        },
      };
    }
    return {
      code: { copy: { onCopy: record }, download: { onDownload: record } },
      table: {
        copy: { onCopy: record },
        download: { onDownload: record },
        csvSeparator: separator,
      },
    };
  }, [mode, record, separator]);

  return (
    <Screen title="Controls" scroll={scroll}>
      <ScrollView
        onScroll={scroll.onScroll}
        scrollEventThrottle={16}
        contentContainerStyle={styles.content}
        keyboardShouldPersistTaps="handled"
      >
        <Hero
          title="Controls"
          summary="Line numbers, fence titles, a capped height, and the copy, download, wrap, and expand controls, all drawn natively."
        />

        <SectionLabel trailing={<Badge label="offline" tone="neutral" />}>
          Configuration
        </SectionLabel>
        <View style={styles.controls}>
          <ChipRow options={MODES} value={mode} onChange={setMode} />
          <ChipRow
            options={STATES}
            value={streamState}
            onChange={setStreamState}
          />
          <View style={styles.row}>
            <Text style={[t.s.caption, styles.rowLabel]}>CSV separator</Text>
          </View>
          <ChipRow
            options={SEPARATORS}
            value={separator}
            onChange={setSeparator}
          />
          <View style={styles.row}>
            <ButtonRow>
              <Button
                label={wrap ? 'Wrap: on' : 'Wrap: off'}
                variant={wrap ? 'primary' : 'secondary'}
                onPress={() => setWrap((value) => !value)}
                hint="Sets codeBlockWrap for the whole document"
              />
              <Button
                label={numbers ? 'Numbers: on' : 'Numbers: off'}
                variant={numbers ? 'primary' : 'secondary'}
                onPress={() => setNumbers((value) => !value)}
                hint="Sets lineNumbers for the whole document"
              />
              <Button
                label={maxHeight === 'none' ? 'Cap: none' : `Cap: ${maxHeight}`}
                variant={maxHeight === 'none' ? 'secondary' : 'primary'}
                onPress={() =>
                  setMaxHeight((value) =>
                    value === 'none'
                      ? 400
                      : value === 400
                      ? 200
                      : value === 200
                      ? 'none'
                      : 400
                  )
                }
                hint="Sets codeBlockMaxHeight for the whole document"
              />
              <Button
                label="Clear log"
                variant="ghost"
                onPress={clear}
                hint="Empties the action log"
              />
            </ButtonRow>
          </View>
          {mode === 'off' && (
            <View style={styles.notice}>
              <Notice tone="accent" title="Controls off">
                The code header and table toolbar are gone. Every block keeps
                the layout it had.
              </Notice>
            </View>
          )}
          {streamState === 'open' && (
            <View style={styles.notice}>
              <Notice tone="accent" title="Held mid-stream">
                The last fence never closes, so its controls stay off. Every
                block above it is settled and its controls work.
              </Notice>
            </View>
          )}
          {mode === 'titled' && (
            <View style={styles.notice}>
              <Notice tone="accent" title="Custom file names">
                Downloads save as snippet plus the block&apos;s extension, and
                rows plus its own. The extension still comes from the block, not
                from the name.
              </Notice>
            </View>
          )}
          {mode === 'no-expand' && (
            <View style={styles.notice}>
              <Notice tone="accent" title="No expand button">
                Only the expand control is off. Copy, download, and wrap are
                unchanged, and a capped block still scrolls inside its card.
              </Notice>
            </View>
          )}
        </View>

        <SectionLabel>Document</SectionLabel>
        <View style={[t.s.card, styles.card]}>
          {stream ? (
            <DownstreamMarkdown
              stream={stream}
              controls={controls()}
              codeBlockWrap={wrap}
              lineNumbers={numbers}
              codeBlockMaxHeight={maxHeight}
              onError={onError}
            />
          ) : (
            <DownstreamMarkdown
              markdown={SOURCE}
              controls={controls()}
              codeBlockWrap={wrap}
              lineNumbers={numbers}
              codeBlockMaxHeight={maxHeight}
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
        <Group title="Control callbacks">
          <Log entries={entries} />
        </Group>

        <View style={styles.foot} />
      </ScrollView>
    </Screen>
  );
}

const styles = StyleSheet.create({
  content: { paddingBottom: space.xxl },
  controls: { paddingBottom: space.md, gap: space.xs },
  row: { paddingHorizontal: space.xl, paddingTop: space.sm },
  rowLabel: { opacity: 0.7 },
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
