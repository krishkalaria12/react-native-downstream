/* eslint-disable no-bitwise -- the native `controls` prop is a bitmask */
/**
 * The code block's height cap, expand control, and label keys, and the
 * normalisation JavaScript does before native sees a value.
 *
 * The pure rules are in `src/types.ts` and `src/controls.ts`, so this is a table
 * of the cases the engine and the platforms have to agree on. The engine's own
 * tables for the gutter width and the card height are in
 * `src/cpp/tests/test_export.cpp`, and the fence meta grammar in
 * `src/cpp/tests/test_fence_meta.cpp`.
 */
import { describe, expect, it, vi } from 'vitest';

import {
  CONTROL_ALL,
  CONTROL_CODE_EXPAND,
  CONTROL_LABEL_KEYS,
  resolveControls,
  shouldShowCodeExpand,
} from '../src/controls';
import { defaultTranslations, resolveMaxHeight } from '../src/types';
import type { MarkdownControls } from '../src/types';

describe('resolveMaxHeight', () => {
  it('is 400 by default, as in Streamdown', () => {
    expect(resolveMaxHeight(undefined)).toBe(400);
  });

  it('takes a number as a height in points', () => {
    expect(resolveMaxHeight(0)).toBe(0);
    expect(resolveMaxHeight(120)).toBe(120);
    expect(resolveMaxHeight(400)).toBe(400);
  });

  it('removes the cap for every "off" value', () => {
    // Streamdown's `resolveMaxHeight` accepts each of these as no cap, and
    // native reads a single 0 as no cap, so the JavaScript side normalises all
    // of them to one value before it crosses.
    expect(resolveMaxHeight('0')).toBe(0);
    expect(resolveMaxHeight('none')).toBe(0);
    expect(resolveMaxHeight('Infinity')).toBe(0);
    expect(resolveMaxHeight(Infinity)).toBe(0);
  });

  it('falls back to the default for a value that is not a height', () => {
    // A CSS length has no meaning in a native layout, and a negative or NaN
    // height would draw a card the caller never asked for. Both warn in
    // development and send the default.
    expect(resolveMaxHeight(-1)).toBe(400);
    expect(resolveMaxHeight(NaN)).toBe(400);
    expect(resolveMaxHeight('50vh' as unknown as number)).toBe(400);
  });
});

describe('the expand control', () => {
  it('is on by default, like every other control', () => {
    expect(shouldShowCodeExpand(true)).toBe(true);
    expect(shouldShowCodeExpand({})).toBe(true);
    expect(shouldShowCodeExpand({ code: {} })).toBe(true);
    expect(resolveControls().flags).toBe(CONTROL_ALL);
  });

  it('turns off on its own, leaving the other code controls alone', () => {
    const flags = resolveControls({ code: { expand: false } }).flags;
    expect(shouldShowCodeExpand({ code: { expand: false } })).toBe(false);
    expect(flags & CONTROL_CODE_EXPAND).toBe(0);
    // The copy, download, and wrap buttons are untouched by this one.
    expect(flags).not.toBe(0);
  });

  it('turns off with the whole code section', () => {
    expect(shouldShowCodeExpand({ code: false })).toBe(false);
    expect(shouldShowCodeExpand(false)).toBe(false);
  });

  it('takes an onToggle callback, as wrap does', () => {
    const onToggle = vi.fn();
    const controls: MarkdownControls = { code: { expand: { onToggle } } };
    expect(resolveControls(controls).handlers.codeExpand).toBe(onToggle);
    // An object form means on, so the button exists even with no callback.
    expect(shouldShowCodeExpand(controls)).toBe(true);
  });
});

describe('the label keys', () => {
  it('appends the expand pair without moving the existing ones', () => {
    // The platforms read this array positionally, so a new key goes at the end
    // rather than shifting every later label by one.
    expect(CONTROL_LABEL_KEYS.length).toBe(21);
    expect(CONTROL_LABEL_KEYS[5]).toBe('expandCode');
    expect(CONTROL_LABEL_KEYS[6]).toBe('collapseCode');
    // Every key before the new pair must still hold what it did.
    expect(CONTROL_LABEL_KEYS[3]).toBe('wrapCode');
    expect(CONTROL_LABEL_KEYS[4]).toBe('unwrapCode');
    expect(CONTROL_LABEL_KEYS[7]).toBe('copyTable');
    // The image keys were appended the same way, after every existing one.
    expect(CONTROL_LABEL_KEYS[16]).toBe('tableFormatTsv');
    expect(CONTROL_LABEL_KEYS[17]).toBe('imageNotAvailable');
    expect(CONTROL_LABEL_KEYS[20]).toBe('imageBlocked');
  });

  it('has a default for each new key', () => {
    expect(defaultTranslations.expandCode).toBe('Show all lines');
    expect(defaultTranslations.collapseCode).toBe('Show fewer lines');
    expect(defaultTranslations.imageNotAvailable).toBe('Image not available');
    expect(defaultTranslations.downloadImage).toBe('Download image');
    expect(defaultTranslations.image).toBe('Image');
    expect(defaultTranslations.imageBlocked).toBe('Image blocked');
  });
});
