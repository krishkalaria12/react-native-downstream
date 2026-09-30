/* eslint-disable no-bitwise -- the native `controls` prop is a bitmask */
/**
 * The `controls` prop, resolved the way Streamdown resolves it, and the events
 * a native control button produces.
 *
 * Resolution is pure and lives in `src/controls.ts`, so most of this is a table
 * of the same cases Streamdown's own `controls.test.ts` and
 * `show-controls.test.tsx` cover. The routing tests at the bottom drive the
 * real view with a stand-in for the native component, so they prove which
 * callback an event reaches rather than what a bitmask holds.
 */
import { describe, expect, it, vi } from 'vitest';

import {
  CONTROL_ALL,
  CONTROL_CODE_COPY,
  CONTROL_CODE_DOWNLOAD,
  CONTROL_CODE_WRAP,
  CONTROL_TABLE_COPY,
  CONTROL_TABLE_DOWNLOAD,
  CONTROL_LABEL_KEYS,
  getDownloadFilename,
  getTableCsvSeparator,
  resolveControls,
  shouldShowCodeWrap,
} from '../src/controls';
import { defaultTranslations } from '../src/types';
import type { MarkdownControls } from '../src/types';

describe('resolveControls', () => {
  it('turns every control on by default', () => {
    expect(resolveControls().flags).toBe(CONTROL_ALL);
    expect(resolveControls(true).flags).toBe(CONTROL_ALL);
    expect(resolveControls({}).flags).toBe(CONTROL_ALL);
    expect(resolveControls({ code: {}, table: {} }).flags).toBe(CONTROL_ALL);
  });

  it('turns every control off for false', () => {
    expect(resolveControls(false).flags).toBe(0);
    expect(shouldShowCodeWrap(false)).toBe(false);
  });

  it('turns one section off', () => {
    const code = resolveControls({ code: false });
    expect(code.flags & CONTROL_CODE_COPY).toBe(0);
    expect(code.flags & CONTROL_CODE_DOWNLOAD).toBe(0);
    expect(code.flags & CONTROL_CODE_WRAP).toBe(0);
    expect(code.flags & CONTROL_TABLE_COPY).toBe(CONTROL_TABLE_COPY);
    expect(code.flags & CONTROL_TABLE_DOWNLOAD).toBe(CONTROL_TABLE_DOWNLOAD);

    const table = resolveControls({ table: false });
    expect(table.flags & CONTROL_TABLE_COPY).toBe(0);
    expect(table.flags & CONTROL_TABLE_DOWNLOAD).toBe(0);
    expect(table.flags & CONTROL_CODE_COPY).toBe(CONTROL_CODE_COPY);
  });

  it('turns one control off inside a section', () => {
    const flags = resolveControls({
      code: { download: false },
      table: { copy: false },
    }).flags;
    expect(flags & CONTROL_CODE_DOWNLOAD).toBe(0);
    expect(flags & CONTROL_TABLE_COPY).toBe(0);
    expect(flags & CONTROL_CODE_COPY).toBe(CONTROL_CODE_COPY);
    expect(flags & CONTROL_TABLE_DOWNLOAD).toBe(CONTROL_TABLE_DOWNLOAD);
  });

  it('turns the wrap button off on its own', () => {
    const flags = resolveControls({ code: { wrap: false } }).flags;
    expect(flags & CONTROL_CODE_WRAP).toBe(0);
    expect(flags & CONTROL_CODE_COPY).toBe(CONTROL_CODE_COPY);
    expect(shouldShowCodeWrap({ code: { wrap: false } })).toBe(false);
    expect(shouldShowCodeWrap({ code: { wrap: { onToggle: () => {} } } })).toBe(
      true
    );
  });

  it('treats a present object as on and an absent key as on', () => {
    expect(resolveControls({ code: true }).flags & CONTROL_CODE_COPY).toBe(
      CONTROL_CODE_COPY
    );
    expect(resolveControls({ code: {} }).flags & CONTROL_CODE_COPY).toBe(
      CONTROL_CODE_COPY
    );
    expect(resolveControls({}).handlers.codeCopy).toBeUndefined();
    expect(resolveControls({}).handlers.tableDownload).toBeUndefined();
  });

  it('resolves a download filename with a fallback for each step', () => {
    // Streamdown's getDownloadFilename: the configured base, or the fallback.
    expect(getDownloadFilename(true, 'code', 'file')).toBe('file');
    expect(getDownloadFilename({}, 'code', 'file')).toBe('file');
    expect(getDownloadFilename({ code: false }, 'code', 'file')).toBe('file');
    expect(
      getDownloadFilename({ code: { download: false } }, 'code', 'file')
    ).toBe('file');
    expect(
      getDownloadFilename(
        { code: { download: { filename: 'snippet' } } },
        'code',
        'file'
      )
    ).toBe('snippet');
    expect(resolveControls().codeFilename).toBe('file');
    expect(resolveControls().tableFilename).toBe('table');
  });

  it('ignores a blank filename and one that is only whitespace', () => {
    const controls: MarkdownControls = {
      code: { download: { filename: '   ' } },
    };
    expect(getDownloadFilename(controls, 'code', 'file')).toBe('file');
    expect(
      getDownloadFilename(
        { code: { download: { filename: '' } } },
        'code',
        'file'
      )
    ).toBe('file');
  });

  it('resolves the CSV separator, rejecting anything unknown', () => {
    expect(getTableCsvSeparator(true)).toBe(',');
    expect(getTableCsvSeparator({})).toBe(',');
    expect(getTableCsvSeparator({ table: true })).toBe(',');
    expect(getTableCsvSeparator({ table: false })).toBe(',');
    expect(getTableCsvSeparator({ table: {} })).toBe(',');
    expect(getTableCsvSeparator({ table: { csvSeparator: ';' } })).toBe(';');
    expect(getTableCsvSeparator({ table: { csvSeparator: '\t' } })).toBe('\t');
    expect(getTableCsvSeparator({ table: { csvSeparator: 'auto' } })).toBe(
      'auto'
    );
    expect(
      getTableCsvSeparator({
        table: { csvSeparator: '|' as unknown as ',' },
      })
    ).toBe(',');
  });

  it('merges translations over the English defaults', () => {
    const labels = resolveControls(true, { copyCode: 'Copiar' }).labels;
    expect(labels[CONTROL_LABEL_KEYS.indexOf('copyCode')]).toBe('Copiar');
    // A key that was not overridden keeps its default.
    expect(labels[CONTROL_LABEL_KEYS.indexOf('downloadFile')]).toBe(
      defaultTranslations.downloadFile
    );
  });

  it('labels every key in the fixed order, and a partial list still works', () => {
    expect(resolveControls().labels).toEqual([
      ...CONTROL_LABEL_KEYS.map((key) => defaultTranslations[key]),
    ]);
    // A caller that sent nothing still gets readable labels rather than blanks.
    expect(
      resolveControls(true, {}).labels.every((label) => label.length > 0)
    ).toBe(true);
  });

  it('collects the callbacks a native event routes to', () => {
    const onCopy = vi.fn();
    const onCopyError = vi.fn();
    const onDownload = vi.fn();
    const onDownloadError = vi.fn();
    const onToggle = vi.fn();
    const tableCopy = vi.fn();
    const tableDownload = vi.fn();
    const resolved = resolveControls({
      code: {
        copy: { onCopy, onError: onCopyError },
        download: { onDownload, onError: onDownloadError },
        wrap: { onToggle },
      },
      table: {
        copy: { onCopy: tableCopy },
        download: { onDownload: tableDownload },
      },
    });
    expect(resolved.handlers.codeCopy).toBe(onCopy);
    expect(resolved.handlers.codeCopyError).toBe(onCopyError);
    expect(resolved.handlers.codeDownload).toBe(onDownload);
    expect(resolved.handlers.codeDownloadError).toBe(onDownloadError);
    expect(resolved.handlers.codeWrap).toBe(onToggle);
    expect(resolved.handlers.tableCopy).toBe(tableCopy);
    expect(resolved.handlers.tableDownload).toBe(tableDownload);
  });

  it('leaves a callback undefined when its section or control is off', () => {
    const onCopy = vi.fn();
    const resolved = resolveControls({
      code: { copy: { onCopy }, download: false },
    });
    expect(resolved.handlers.codeCopy).toBe(onCopy);
    expect(resolved.handlers.codeDownload).toBeUndefined();
    expect(resolveControls({ code: false }).handlers.codeCopy).toBeUndefined();
  });

  it('does not read callbacks off an unrelated section', () => {
    const onCopy = vi.fn();
    // A table's copy callback must not be found under code, and the other way
    // round, or one control's handler would answer another's press.
    const resolved = resolveControls({ table: { copy: { onCopy } } });
    expect(resolved.handlers.tableCopy).toBe(onCopy);
    expect(resolved.handlers.codeCopy).toBeUndefined();
  });
});
