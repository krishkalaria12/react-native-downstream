/**
 * Link resolution and the link-safety confirmation.
 *
 * `src/linkSafety.ts` is the one part of this library that opens something. Both
 * halves of it are pure enough to test without a device: resolution turns a
 * relative reference and an origin into a URL, and the confirmation decides
 * between opening and asking. Neither is allowed to reach for `URL`, whose
 * base-resolution behaviour differs between React Native's polyfill and a
 * browser, so the resolver is spelled out here case by case.
 */
import { beforeEach, describe, expect, it, vi } from 'vitest';

import { Alert, Linking } from 'react-native';

import {
  clipboardAvailable,
  confirmAndOpenLink,
  copyToClipboard,
  linkSafetyLabels,
  openExternalLink,
  openLinkSafely,
  resolveLinkOptions,
  resolveLinkUrl,
  resolvesLinks,
} from '../src/linkSafety';
import NativeDownstreamSession from '../src/NativeDownstreamSession';

const LABELS = {
  title: 'Open this link?',
  openLabel: 'Open',
  copyLabel: 'Copy link',
  cancelLabel: 'Close',
};

describe('resolveLinkUrl', () => {
  it('leaves an absolute URL alone', () => {
    expect(resolveLinkUrl('https://a.example/b', 'https://x.example')).toBe(
      'https://a.example/b'
    );
    expect(resolveLinkUrl('mailto:a@b', 'https://x.example')).toBe(
      'mailto:a@b'
    );
  });

  it('leaves it alone with no origin to resolve against', () => {
    expect(resolveLinkUrl('/docs/a', undefined)).toBe('/docs/a');
    expect(resolveLinkUrl('/docs/a', '')).toBe('/docs/a');
  });

  const origin = 'https://x.example';

  const cases: ReadonlyArray<readonly [string, string]> = [
    ['/docs/a', 'https://x.example/docs/a'],
    ['#section', 'https://x.example/#section'],
    ['?q=1', 'https://x.example/?q=1'],
    ['/a#b', 'https://x.example/a#b'],
    ['/a?q=1#b', 'https://x.example/a?q=1#b'],
    ['./a', 'https://x.example/a'],
    ['./a/b', 'https://x.example/a/b'],
    ['../a', 'https://x.example/a'],
    ['../../a', 'https://x.example/a'],
    // A `..` past the root clamps, as a browser's and `URL`'s both do.
    ['../../../a', 'https://x.example/a'],
    // A `.` inside a path is removed; a trailing one keeps the directory.
    ['./a/./b', 'https://x.example/a/b'],
    ['./a/..', 'https://x.example/'],
    ['./a/../', 'https://x.example/'],
  ];

  for (const [href, expected] of cases) {
    it(`${href} → ${expected}`, () => {
      expect(resolveLinkUrl(href, origin)).toBe(expected);
    });
  }

  it('accepts an origin with no scheme, and one with a trailing slash', () => {
    expect(resolveLinkUrl('/a', 'x.example')).toBe('https://x.example/a');
    expect(resolveLinkUrl('/a', 'https://x.example/')).toBe(
      'https://x.example/a'
    );
    expect(resolveLinkUrl('/a', '  https://x.example  ')).toBe(
      'https://x.example/a'
    );
  });

  it('does not resolve a bare path, which the link policy never links', () => {
    // The parser refuses `docs/a` even with the relative bit on, so the resolver
    // has nothing to do with one. Resolving it anyway would turn a guard in the
    // parser into a second, different rule.
    expect(resolveLinkUrl('docs/a', origin)).toBe('docs/a');
  });
});

describe('resolvesLinks', () => {
  it('is off unless the app asked', () => {
    expect(resolvesLinks(undefined)).toBe(false);
    expect(resolvesLinks({})).toBe(false);
    expect(resolvesLinks({ relative: false })).toBe(false);
    expect(resolvesLinks({ relative: true })).toBe(true);
    expect(resolvesLinks({ defaultOrigin: 'https://x' })).toBe(true);
  });

  it('sends only the trimmed origin to native', () => {
    expect(resolveLinkOptions(undefined)).toEqual({ origin: '' });
    expect(
      resolveLinkOptions({ defaultOrigin: '  https://x.example  ' })
    ).toEqual({ origin: 'https://x.example' });
  });
});

describe('openExternalLink', () => {
  beforeEach(() => {
    vi.clearAllMocks();
  });

  it('asks the platform first and opens what it accepts', async () => {
    vi.mocked(Linking.canOpenURL).mockResolvedValue(true);
    expect(await openExternalLink('https://a.example')).toBe(true);
    expect(Linking.canOpenURL).toHaveBeenCalledWith('https://a.example');
    expect(Linking.openURL).toHaveBeenCalledWith('https://a.example');
  });

  it('reports false rather than opening what nothing handles', async () => {
    vi.mocked(Linking.canOpenURL).mockResolvedValue(false);
    expect(await openExternalLink('myapp://x')).toBe(false);
    expect(Linking.openURL).not.toHaveBeenCalled();
  });

  it('survives a platform that throws', async () => {
    vi.mocked(Linking.canOpenURL).mockRejectedValue(new Error('no handler'));
    expect(await openExternalLink('https://a.example')).toBe(false);
  });
});

describe('the clipboard', () => {
  it('reports availability and copies through the TurboModule', async () => {
    expect(clipboardAvailable()).toBe(true);
    expect(await copyToClipboard('https://a.example')).toBe(true);
    expect(NativeDownstreamSession.copyText).toHaveBeenCalledWith(
      'https://a.example'
    );
  });

  it('reports false when the platform has no clipboard', async () => {
    vi.mocked(NativeDownstreamSession.copyText).mockRejectedValueOnce(
      new Error('no clipboard')
    );
    expect(await copyToClipboard('x')).toBe(false);
  });
});

describe('confirmAndOpenLink', () => {
  beforeEach(() => {
    vi.clearAllMocks();
    vi.mocked(Linking.canOpenURL).mockResolvedValue(true);
  });

  it('opens a trusted URL without asking', async () => {
    await confirmAndOpenLink(
      'https://a.example',
      LABELS,
      () => true,
      undefined
    );
    expect(Linking.openURL).toHaveBeenCalledWith('https://a.example');
    expect(Alert.alert).not.toHaveBeenCalled();
  });

  it('asks about an untrusted one', async () => {
    await confirmAndOpenLink(
      'https://a.example',
      LABELS,
      () => false,
      undefined
    );
    expect(Alert.alert).toHaveBeenCalled();
    expect(Linking.openURL).not.toHaveBeenCalled();
  });

  it('awaits a promise-returning check', async () => {
    await confirmAndOpenLink(
      'https://a.example',
      LABELS,
      async () => true,
      undefined
    );
    expect(Linking.openURL).toHaveBeenCalled();
  });

  it('treats a check that throws as a check that did not pass', async () => {
    // A network failure is the one case that must not skip the confirmation.
    await confirmAndOpenLink(
      'https://a.example',
      LABELS,
      () => {
        throw new Error('offline');
      },
      undefined
    );
    expect(Linking.openURL).not.toHaveBeenCalled();
    expect(Alert.alert).toHaveBeenCalled();
  });

  it('gives a custom modal the URL and the four strings', async () => {
    const renderModal = vi.fn();
    await confirmAndOpenLink(
      'https://a.example',
      LABELS,
      () => false,
      renderModal
    );
    const props = renderModal.mock.calls[0]?.[0];
    expect(props?.url).toBe('https://a.example');
    expect(props.title).toBe('Open this link?');
    expect(props.openLabel).toBe('Open');
    expect(props.copyLabel).toBe('Copy link');
    expect(props.cancelLabel).toBe('Close');
    expect(Alert.alert).not.toHaveBeenCalled();
  });

  it('opens when the reader confirms, and not when they cancel', async () => {
    const renderModal = vi.fn();
    await confirmAndOpenLink(
      'https://a.example',
      LABELS,
      () => false,
      renderModal
    );
    const props = renderModal.mock.calls[0]?.[0];
    props?.onCancel();
    await props?.onConfirm();
    // `onConfirm` hands the open to `Linking` without awaiting it, because a
    // modal's own button has no caller to wait on, so the test gives the
    // microtask queue a turn before looking.
    await Promise.resolve();
    expect(Linking.openURL).toHaveBeenCalledWith('https://a.example');
  });

  it('the native alert opens, copies or cancels as the reader chooses', async () => {
    // The buttons are Cancel, Copy and Open, in the order a reader reads them,
    // and the last two are indices the test presses.
    const alert = Alert as unknown as { answer?: number };
    try {
      alert.answer = 2;
      await confirmAndOpenLink(
        'https://a.example',
        LABELS,
        () => false,
        undefined
      );
      expect(Linking.openURL).toHaveBeenCalledWith('https://a.example');

      vi.clearAllMocks();
      alert.answer = 1;
      await confirmAndOpenLink(
        'https://b.example',
        LABELS,
        () => false,
        undefined
      );
      expect(Linking.openURL).not.toHaveBeenCalled();
      expect(NativeDownstreamSession.copyText).toHaveBeenCalledWith(
        'https://b.example'
      );

      vi.clearAllMocks();
      alert.answer = 0;
      await confirmAndOpenLink(
        'https://c.example',
        LABELS,
        () => false,
        undefined
      );
      expect(Linking.openURL).not.toHaveBeenCalled();
      expect(NativeDownstreamSession.copyText).not.toHaveBeenCalled();
    } finally {
      delete alert.answer;
    }
  });
});

describe('openLinkSafely', () => {
  beforeEach(() => {
    vi.clearAllMocks();
    vi.mocked(Linking.canOpenURL).mockResolvedValue(true);
  });

  it('resolves a relative destination against the origin before opening', async () => {
    await openLinkSafely('../docs/a', {
      enabled: true,
      origin: 'https://x.example',
      onLinkCheck: () => true,
    });
    expect(Linking.openURL).toHaveBeenCalledWith('https://x.example/docs/a');
  });

  it('opens straight through with safety off', async () => {
    await openLinkSafely('https://a.example', {
      enabled: false,
      origin: '',
    });
    expect(Linking.openURL).toHaveBeenCalledWith('https://a.example');
    expect(Alert.alert).not.toHaveBeenCalled();
  });

  it("asks with the app's own strings", async () => {
    await openLinkSafely('https://a.example', {
      enabled: true,
      origin: '',
      translations: {
        externalLinkWarning: 'Ouvre ce lien ?',
        openLink: 'Ouvrir',
        copyLink: 'Copier',
        close: 'Fermer',
      },
    });
    expect(Alert.alert).toHaveBeenCalledWith(
      'Ouvre ce lien ?',
      'https://a.example',
      expect.anything(),
      expect.anything()
    );
  });
});

describe('linkSafetyLabels', () => {
  it('falls back to the English defaults, key by key', () => {
    expect(linkSafetyLabels({})).toEqual({
      title: 'Open this link?',
      openLabel: 'Open',
      copyLabel: 'Copy link',
      cancelLabel: 'Close',
    });
    expect(linkSafetyLabels({ openLink: 'Ouvrir' })).toEqual({
      title: 'Open this link?',
      openLabel: 'Ouvrir',
      copyLabel: 'Copy link',
      cancelLabel: 'Close',
    });
  });
});
