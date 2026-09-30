import { useCallback, useState } from 'react';
import { ScrollView, StyleSheet, Text, View } from 'react-native';
import {
  DownstreamMarkdown,
  type MarkdownControlEvent,
  type MarkdownControls,
  type MarkdownErrorEvent,
} from 'react-native-downstream';

import { Badge, Button, ButtonRow, ChipRow, Group, Notice } from '../ui/kit';
import { Hero, Screen, SectionLabel, useScreenScroll } from '../ui/screen';
import { radius, space, useTheme } from '../ui/theme';

/**
 * A document with one of everything a control can act on: a short fence, a long
 * fence that needs wrapping, an indented block with no language, and a table
 * with an alignment row.
 */
const SOURCE = `# Copy and download controls

Controls are on by default. Every button below was drawn by the native view; the
log records what each press reported.

## A short fence

\`\`\`ts
export const answer = 42;
\`\`\`

## A fence that needs wrapping

\`\`\`ts
const configuration = { retries: 3, backoffMs: 250, timeoutMs: 30_000, endpoint: 'https://example.com/v1/segments', headers: { accept: 'application/json' } };
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

const MODES = [
  { value: 'defaults', label: 'Defaults' },
  { value: 'titled', label: 'Custom names' },
  { value: 'off', label: 'Controls off' },
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
          summary="Copy, download, and wrap on code blocks and tables, drawn natively and enabled by default."
        />

        <SectionLabel trailing={<Badge label="offline" tone="neutral" />}>
          Configuration
        </SectionLabel>
        <View style={styles.controls}>
          <ChipRow options={MODES} value={mode} onChange={setMode} />
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
          {mode === 'titled' && (
            <View style={styles.notice}>
              <Notice tone="accent" title="Custom file names">
                Downloads save as snippet plus the block&apos;s extension, and
                rows plus its own. The extension still comes from the block, not
                from the name.
              </Notice>
            </View>
          )}
        </View>

        <SectionLabel>Document</SectionLabel>
        <View style={[t.s.card, styles.card]}>
          <DownstreamMarkdown
            markdown={SOURCE}
            controls={controls()}
            codeBlockWrap={wrap}
            onError={onError}
          />
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
  log: { gap: space.xs },
  logEmpty: { padding: space.lg },
  logLine: { opacity: 0.75 },
  foot: { height: space.xxl },
});
