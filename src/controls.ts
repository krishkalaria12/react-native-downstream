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
export const CONTROL_CODE_EXPAND = 1 << 5;
export const CONTROL_IMAGE_DOWNLOAD = 1 << 6;
/** Per-table expand, which lifts `tableMaxHeight` for one table. */
export const CONTROL_TABLE_EXPAND = 1 << 7;
/**
 * Per-table full screen. `controls.table.fullscreen` is Streamdown's key.
 *
 * The button opens a JavaScript modal holding a second view rather than a
 * native screen, so there is no new native drawing behind it — but it changes
 * nothing about a table's height either, for the same reason the image
 * download button does not: the table keeps its cap.
 */
export const CONTROL_TABLE_FULLSCREEN = 1 << 8;

/** Every control on, which is the default and the codegen default. */
export const CONTROL_ALL =
  CONTROL_CODE_COPY |
  CONTROL_CODE_DOWNLOAD |
  CONTROL_TABLE_COPY |
  CONTROL_TABLE_DOWNLOAD |
  CONTROL_CODE_WRAP |
  CONTROL_CODE_EXPAND |
  CONTROL_IMAGE_DOWNLOAD |
  CONTROL_TABLE_EXPAND |
  CONTROL_TABLE_FULLSCREEN;

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
  // Appended rather than inserted: the platforms read this array positionally,
  // so a new key goes at the end rather than moving every later one.
  'expandCode',
  'collapseCode',
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
  'imageNotAvailable',
  'downloadImage',
  'image',
  'imageBlocked',
  // Phase 1: the link-safety confirmation.
  'externalLinkWarning',
  'openLink',
  'copyLink',
  'close',
  // Phase 2: the table's two new buttons.
  'viewFullscreen',
  'exitFullscreen',
  'expandTable',
  'collapseTable',
  // Phase 5: the details summary's chevron and its accessible name.
  'expandDetails',
  'collapseDetails',
] as const satisfies readonly (keyof MarkdownTranslations)[];

/** One handler per button, so routing an event is a table lookup. */
export type ControlHandlers = Readonly<{
  codeCopy?: (event: MarkdownControlEvent) => void;
  codeCopyError?: (error: MarkdownErrorEvent) => void;
  codeDownload?: (event: MarkdownControlEvent) => void;
  codeDownloadError?: (error: MarkdownErrorEvent) => void;
  codeWrap?: (event: MarkdownControlEvent) => void;
  codeExpand?: (event: MarkdownControlEvent) => void;
  tableCopy?: (event: MarkdownControlEvent) => void;
  tableCopyError?: (error: MarkdownErrorEvent) => void;
  tableDownload?: (event: MarkdownControlEvent) => void;
  tableDownloadError?: (error: MarkdownErrorEvent) => void;
  tableExpand?: (event: MarkdownControlEvent) => void;
  tableFullscreen?: (event: MarkdownControlEvent) => void;
  imageDownload?: (event: MarkdownControlEvent) => void;
  imageDownloadError?: (error: MarkdownErrorEvent) => void;
}>;

export type ResolvedControls = Readonly<{
  /** Bitmask for the native `controls` prop. */
  flags: number;
  codeFilename: string;
  tableFilename: string;
  imageFilename: string;
  csvSeparator: CsvSeparator;
  /** Label strings in `CONTROL_LABEL_KEYS` order. */
  labels: readonly string[];
  handlers: ControlHandlers;
}>;

type Section = Readonly<Record<string, unknown>>;

function section(
  controls: MarkdownControls,
  key: 'code' | 'table' | 'image'
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
  key: 'code' | 'table' | 'image'
): boolean {
  if (typeof controls === 'boolean') return controls;
  // `null` is not one of the shapes `MarkdownControls` allows, but a value from
  // JSON or a config file can be one, and reading a key off it would throw where
  // every other accessor here treats it as absent.
  if (typeof controls !== 'object' || controls === null) return true;
  return (controls as Section)[key] !== false;
}

/**
 * True when one control inside a section is on. A boolean or a missing key
 * means on and only an explicit `false` turns it off, which is how Streamdown
 * reads `{ code: { download: false } }`.
 */
function controlEnabled(
  controls: MarkdownControls,
  key: 'code' | 'table' | 'image',
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

/** True when the code header's per-block expand button is on. */
export function shouldShowCodeExpand(controls: MarkdownControls): boolean {
  if (typeof controls === 'boolean') return controls;
  if (!sectionEnabled(controls, 'code')) return false;
  return section(controls, 'code')?.expand !== false;
}

/** True when the table toolbar's per-table expand button is on. */
export function shouldShowTableExpand(controls: MarkdownControls): boolean {
  if (typeof controls === 'boolean') return controls;
  if (!sectionEnabled(controls, 'table')) return false;
  return section(controls, 'table')?.expand !== false;
}

/** True when the table toolbar's full-screen button is on. */
export function shouldShowTableFullscreen(controls: MarkdownControls): boolean {
  if (typeof controls === 'boolean') return controls;
  if (!sectionEnabled(controls, 'table')) return false;
  return section(controls, 'table')?.fullscreen !== false;
}

/** A control's object form, or undefined when it is a boolean or absent. */
function detail(
  controls: MarkdownControls,
  key: 'code' | 'table' | 'image',
  control: 'copy' | 'download' | 'wrap' | 'expand' | 'fullscreen'
): Section | undefined {
  return section(controls, key)?.[control] as Section | undefined;
}

function of<T>(settings: Section | undefined, key: string): T | undefined {
  return settings?.[key] as T | undefined;
}

/** Streamdown's `getDownloadFilename`, with `fallback` for "not configured". */
export function getDownloadFilename(
  controls: MarkdownControls,
  type: 'code' | 'table' | 'image',
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
  if (controlEnabled(controls, 'image', 'download')) {
    flags |= CONTROL_IMAGE_DOWNLOAD;
  }
  if (shouldShowCodeWrap(controls)) flags |= CONTROL_CODE_WRAP;
  if (shouldShowCodeExpand(controls)) flags |= CONTROL_CODE_EXPAND;
  if (shouldShowTableExpand(controls)) flags |= CONTROL_TABLE_EXPAND;
  if (shouldShowTableFullscreen(controls)) flags |= CONTROL_TABLE_FULLSCREEN;

  const codeCopy = detail(controls, 'code', 'copy');
  const codeDownload = detail(controls, 'code', 'download');
  const codeWrap = detail(controls, 'code', 'wrap');
  const codeExpand = detail(controls, 'code', 'expand');
  const tableCopy = detail(controls, 'table', 'copy');
  const tableDownload = detail(controls, 'table', 'download');
  const tableExpand = detail(controls, 'table', 'expand');
  const tableFullscreen = detail(controls, 'table', 'fullscreen');
  const imageDownload = detail(controls, 'image', 'download');

  return {
    flags,
    codeFilename: getDownloadFilename(controls, 'code', 'file'),
    tableFilename: getDownloadFilename(controls, 'table', 'table'),
    imageFilename: getDownloadFilename(controls, 'image', 'image'),
    csvSeparator: getTableCsvSeparator(controls),
    labels,
    handlers: {
      codeCopy: of(codeCopy, 'onCopy'),
      codeCopyError: of(codeCopy, 'onError'),
      codeDownload: of(codeDownload, 'onDownload'),
      codeDownloadError: of(codeDownload, 'onError'),
      codeWrap: of(codeWrap, 'onToggle'),
      codeExpand: of(codeExpand, 'onToggle'),
      tableCopy: of(tableCopy, 'onCopy'),
      tableCopyError: of(tableCopy, 'onError'),
      tableDownload: of(tableDownload, 'onDownload'),
      tableDownloadError: of(tableDownload, 'onError'),
      tableExpand: of(tableExpand, 'onToggle'),
      tableFullscreen: of(tableFullscreen, 'onToggle'),
      imageDownload: of(imageDownload, 'onDownload'),
      imageDownloadError: of(imageDownload, 'onError'),
    },
  };
}
