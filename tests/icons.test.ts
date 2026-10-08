/**
 * The `icons` prop, resolved to names the platforms draw.
 *
 * A glyph is named rather than shipped because the platform's own symbol is the
 * right answer on each platform and an image cannot cross into a native button's
 * header. These cases pin the two decisions that follow from that: what a key with
 * no entry sends, and what an entry the platform does not know becomes.
 */
import { describe, expect, it } from 'vitest';

import { iconIndex, resolveIcon, resolveIcons } from '../src/icons';
import { CONTROL_ICON_KEYS } from '../src/types';

describe('resolveIcons', () => {
  it('sends one entry per key, in the fixed order the platforms read', () => {
    // Positional, beside `controlLabels`, so the order is part of the contract
    // rather than an implementation detail.
    expect(resolveIcons().length).toBe(CONTROL_ICON_KEYS.length);
    expect(CONTROL_ICON_KEYS).toEqual([
      'copy',
      'check',
      'download',
      'maximize',
      'close',
      'wrap',
      'expand',
      'collapse',
      'tableFullscreen',
      'detailsChevron',
    ]);
  });

  it('sends nothing for a key the app did not name', () => {
    // An empty string is "the platform's own glyph", not "no glyph": the
    // platform owns its defaults, so naming one here would be a second set of them.
    expect(resolveIcons()).toEqual(CONTROL_ICON_KEYS.map(() => ''));
    expect(resolveIcons({})).toEqual(CONTROL_ICON_KEYS.map(() => ''));
    expect(resolveIcons({ copy: { name: 'square.and.arrow.up' } })).toEqual([
      'square.and.arrow.up',
      '',
      '',
      '',
      '',
      '',
      '',
      '',
      '',
      '',
    ]);
  });

  it('keeps a name as written, trimmed', () => {
    expect(resolveIcon({ name: '  doc.on.doc  ' })).toBe('doc.on.doc');
  });

  it('reads an image source as a URI the loader can fetch', () => {
    // A bundled image has no stable path in a release build, so the bytes travel
    // with the name rather than being looked up later.
    expect(resolveIcon({ image: { uri: 'my_copy' } })).toBe('asset:/my_copy');
    // A remote one is already what the loader reads.
    expect(resolveIcon({ image: { uri: 'https://example.com/c.png' } })).toBe(
      'https://example.com/c.png'
    );
    expect(
      resolveIcon({ image: { uri: 'data:image/svg+xml;base64,AA' } })
    ).toBe('data:image/svg+xml;base64,AA');
  });

  it('sends an empty name for an entry that names nothing', () => {
    // A text-only button is a thing an app wants on a quiet header, so an entry
    // that names nothing is honoured rather than falling back to the default.
    expect(resolveIcon({ name: '   ' })).toBe('');
    expect(resolveIcon({ image: { uri: '' } })).toBe('');
    expect(resolveIcon(undefined)).toBe('');
  });

  it('locates a key by its position, for a caller reading one glyph', () => {
    expect(iconIndex('copy')).toBe(0);
    expect(iconIndex('detailsChevron')).toBe(CONTROL_ICON_KEYS.length - 1);
  });
});
