/* eslint-disable no-bitwise -- the native `controls` prop is a bitmask */
/**
 * Resolution of the `controls` prop.
 *
 * Streamdown resolves `controls` with three small helpers; a value written for
 * its web component is reused here as it is, so the same shape and the same
 * "a missing key means on" rule decide what the native header and toolbar
 * draw. Everything here is pure: the functions stay in JavaScript and native
 * only ever sees a bitmask, two file-name strings, one separator, and a fixed
 * array of label strings.
 */
import {
  defaultTranslations,
  type MarkdownControlEvent,
  type MarkdownControls,
  type MarkdownErrorEvent,
  type MarkdownTranslations,
} from './types';

/** Bits of the native `controls` prop, in a fixed order. */
export const CONTROL_CODE_COPY = 1 << 0;
export const CONTROL_CODE_DOWNLOAD = 1 << 1;
export const CONTROL_TABLE_COPY = 1 << 2;
export const CONTROL_TABLE_DOWNLOAD = 1 << 3;
export const CONTROL_CODE_WRAP = 1 << 4;

/** Every control on, which is the default and the codegen default. */
export const CONTROL_ALL =
  CONTROL_CODE_COPY |
  CONTROL_CODE_DOWNLOAD |
  CONTROL_TABLE_COPY |
  CONTROL_TABLE_DOWNLOAD |
  CONTROL_CODE_WRAP;

export type CsvSeparator = ',' | ';' | '\t' | 'auto';

const CSV_SEPARATORS: ReadonlySet<string> = new Set([',', ';', '\t', 'auto']);

/**
 * Label strings in the order the native `controlLabels` prop carries them.
 * Fixed, so the platforms read them positionally and a new key is a new
 * constant here rather than a reshape of the array.
 */
export const CONTROL_LABEL_KEYS = [
  'copyCode',
  'downloadFile',
  'copied',
  'wrapCode',
  'unwrapCode',
  'copyTable',
  'copyTableAsMarkdown',
  'copyTableAsCsv',
  'copyTableAsTsv',
  'downloadTable',
  'downloadTableAsCsv',
  'downloadTableAsMarkdown',
  'tableFormatMarkdown',
  'tableFormatCsv',
  'tableFormatTsv',
] as const satisfies readonly (keyof MarkdownTranslations)[];

/** One handler per button, so routing an event is a table lookup. */
export type ControlHandlers = Readonly<{
  codeCopy?: (event: MarkdownControlEvent) => void;
  codeCopyError?: (error: MarkdownErrorEvent) => void;
  codeDownload?: (event: MarkdownControlEvent) => void;
  codeDownloadError?: (error: MarkdownErrorEvent) => void;
  codeWrap?: (event: MarkdownControlEvent) => void;
  tableCopy?: (event: MarkdownControlEvent) => void;
  tableCopyError?: (error: MarkdownErrorEvent) => void;
  tableDownload?: (event: MarkdownControlEvent) => void;
  tableDownloadError?: (error: MarkdownErrorEvent) => void;
}>;

export type ResolvedControls = Readonly<{
  /** Bitmask for the native `controls` prop. */
  flags: number;
  codeFilename: string;
  tableFilename: string;
  csvSeparator: CsvSeparator;
  /** Label strings in `CONTROL_LABEL_KEYS` order. */
  labels: readonly string[];
  handlers: ControlHandlers;
}>;

type Section = Readonly<Record<string, unknown>>;

function section(
  controls: MarkdownControls,
  key: 'code' | 'table'
): Section | undefined {
  if (typeof controls !== 'object' || controls === null) return undefined;
  const value = (controls as Section)[key];
  return typeof value === 'object' && value !== null
    ? (value as Section)
    : undefined;
}

/** True when a section is absent or not `false`, so `true` and `{}` both mean on. */
function sectionEnabled(
  controls: MarkdownControls,
  key: 'code' | 'table'
): boolean {
  if (typeof controls === 'boolean') return controls;
  return (controls as Section)[key] !== false;
}

/**
 * True when one control inside a section is on. A boolean or a missing key
 * means on and only an explicit `false` turns it off, which is how Streamdown
 * reads `{ code: { download: false } }`.
 */
function controlEnabled(
  controls: MarkdownControls,
  key: 'code' | 'table',
  control: 'copy' | 'download'
): boolean {
  if (typeof controls === 'boolean') return controls;
  if (!sectionEnabled(controls, key)) return false;
  return section(controls, key)?.[control] !== false;
}

/** True when the code header's per-block wrap button is on. */
export function shouldShowCodeWrap(controls: MarkdownControls): boolean {
  if (typeof controls === 'boolean') return controls;
  if (!sectionEnabled(controls, 'code')) return false;
  return section(controls, 'code')?.wrap !== false;
}

/** A control's object form, or undefined when it is a boolean or absent. */
function detail(
  controls: MarkdownControls,
  key: 'code' | 'table',
  control: 'copy' | 'download' | 'wrap'
): Section | undefined {
  return section(controls, key)?.[control] as Section | undefined;
}

function of<T>(settings: Section | undefined, key: string): T | undefined {
  return settings?.[key] as T | undefined;
}

/** Streamdown's `getDownloadFilename`, with `fallback` for "not configured". */
export function getDownloadFilename(
  controls: MarkdownControls,
  type: 'code' | 'table',
  fallback: string
): string {
  return (
    of<string>(detail(controls, type, 'download'), 'filename')?.trim() ||
    fallback
  );
}

/** Streamdown's `getTableCsvSeparator`, which has no `auto` resolution here. */
export function getTableCsvSeparator(controls: MarkdownControls): CsvSeparator {
  const configured = section(controls, 'table')?.csvSeparator;
  return typeof configured === 'string' && CSV_SEPARATORS.has(configured)
    ? (configured as CsvSeparator)
    : ',';
}

/**
 * Resolves `controls` into everything the view and the native side need.
 *
 * `translations` is merged over the English defaults here rather than on each
 * platform, so a button, a menu item, and an announcement read the same string
 * for a key an app overrode.
 */
export function resolveControls(
  controls: MarkdownControls = true,
  translations: Partial<MarkdownTranslations> = {}
): ResolvedControls {
  const labels = CONTROL_LABEL_KEYS.map(
    (key) => translations[key] ?? defaultTranslations[key]
  );

  let flags = 0;
  if (controlEnabled(controls, 'code', 'copy')) flags |= CONTROL_CODE_COPY;
  if (controlEnabled(controls, 'code', 'download')) {
    flags |= CONTROL_CODE_DOWNLOAD;
  }
  if (controlEnabled(controls, 'table', 'copy')) flags |= CONTROL_TABLE_COPY;
  if (controlEnabled(controls, 'table', 'download')) {
    flags |= CONTROL_TABLE_DOWNLOAD;
  }
  if (shouldShowCodeWrap(controls)) flags |= CONTROL_CODE_WRAP;

  const codeCopy = detail(controls, 'code', 'copy');
  const codeDownload = detail(controls, 'code', 'download');
  const codeWrap = detail(controls, 'code', 'wrap');
  const tableCopy = detail(controls, 'table', 'copy');
  const tableDownload = detail(controls, 'table', 'download');

  return {
    flags,
    codeFilename: getDownloadFilename(controls, 'code', 'file'),
    tableFilename: getDownloadFilename(controls, 'table', 'table'),
    csvSeparator: getTableCsvSeparator(controls),
    labels,
    handlers: {
      codeCopy: of(codeCopy, 'onCopy'),
      codeCopyError: of(codeCopy, 'onError'),
      codeDownload: of(codeDownload, 'onDownload'),
      codeDownloadError: of(codeDownload, 'onError'),
      codeWrap: of(codeWrap, 'onToggle'),
      tableCopy: of(tableCopy, 'onCopy'),
      tableCopyError: of(tableCopy, 'onError'),
      tableDownload: of(tableDownload, 'onDownload'),
      tableDownloadError: of(tableDownload, 'onError'),
    },
  };
}
