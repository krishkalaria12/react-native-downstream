/**
 * `syntaxColorsFromTheme`: a VS Code or Shiki theme's JSON onto the engine's
 * token classes.
 *
 * The golden that matters is `github-light-default` and `github-dark-default`:
 * converting those two must reproduce the engine's own token palette exactly,
 * because that palette *is* those themes. A third theme has a checked-in golden
 * so a change to the scope matching shows up as a diff rather than as a
 * different-looking code block nobody noticed.
 */
import { readFileSync } from 'node:fs';
import { join } from 'node:path';

import { describe, expect, it } from 'vitest';

import { syntaxColorsFromTheme, TOKEN_CLASSES } from '../src/syntaxTheme';
import type { VSCodeThemeJson } from '../src/vscodeTheme';

function theme(name: string): VSCodeThemeJson {
  return JSON.parse(
    readFileSync(join(__dirname, 'fixtures', `${name}.json`), 'utf8')
  ) as VSCodeThemeJson;
}

const githubLight = theme('shiki-github-light-default');
const githubDark = theme('shiki-github-dark-default');
const dracula = theme('shiki-dracula');

/**
 * The engine's `TokenPalette.h` palette, as ARGB. Kept here rather than read from
 * the header so the JavaScript test proves the converter's own output; the C++
 * test proves the header and the converter agree, which together is what makes
 * "the default did not change" checkable from both sides.
 */
const ENGINE_PALETTE: Readonly<Record<string, [number, number]>> = {
  plain: [0xff1f2328, 0xfff0f6fc],
  comment: [0xff6e7781, 0xff8b949e],
  keyword: [0xffcf222e, 0xffff7b72],
  type: [0xff8250df, 0xffd2a8ff],
  string: [0xff0a3069, 0xffa5d6ff],
  number: [0xff0550ae, 0xff79c0ff],
  constant: [0xff0550ae, 0xff79c0ff],
  regex: [0xff116329, 0xff7ee787],
  function: [0xff8250df, 0xffd2a8ff],
  property: [0xff0550ae, 0xff79c0ff],
  operator: [0xff1f2328, 0xfff0f6fc],
  preprocessor: [0xff953800, 0xffffa657],
  tag: [0xff116329, 0xff7ee787],
  attribute: [0xff0550ae, 0xff79c0ff],
  inserted: [0xff116329, 0xff7ee787],
  deleted: [0xff82071e, 0xffffa198],
  heading: [0xff0550ae, 0xff79c0ff],
};

describe('syntaxColorsFromTheme', () => {
  it('reproduces the engine palette from GitHub light and dark', () => {
    // The default code colors *are* these two themes, so a converter that could
    // not read them back would break every code block in the default rendering.
    const light = syntaxColorsFromTheme(githubLight).syntax;
    const dark = syntaxColorsFromTheme(githubDark).syntax;
    for (const token of TOKEN_CLASSES) {
      if (token === 'plain' || token === 'operator') {
        // Both take the code block's own text color, which the theme reaches
        // through `codeBlock.color`, so the converter leaves them out rather
        // than reading a `variable.other` the engine already gets right.
        expect(light[token], `${token} light`).toBeUndefined();
        expect(dark[token], `${token} dark`).toBeUndefined();
        continue;
      }
      // Named rather than indexed blindly, so a token the palette lost fails as
      // "no palette for comment" instead of as a comparison against nothing.
      const palette = ENGINE_PALETTE[token];
      expect(palette, `${token} has an engine color`).toBeDefined();
      if (palette === undefined) {
        continue;
      }
      expect(light[token], `${token} light`).toBe(hex(palette[0]));
      expect(dark[token], `${token} dark`).toBe(hex(palette[1]));
    }
  });

  it('reads the code block colors from the editor colors', () => {
    expect(syntaxColorsFromTheme(githubLight).codeBlock).toEqual({
      backgroundColor: '#ffffff',
      color: '#1f2328',
    });
    expect(syntaxColorsFromTheme(githubDark).codeBlock).toEqual({
      backgroundColor: '#0d1117',
      color: '#e6edf3',
    });
  });

  it('matches a golden for a third theme', () => {
    // A checked-in golden rather than a second hand-written table, so a change
    // to the scope matching is a diff a reviewer can read.
    expect(syntaxColorsFromTheme(dracula).syntax).toMatchInlineSnapshot(`
      {
        "attribute": "#8be9fd",
        "comment": "#6272a4",
        "constant": "#bd93f9",
        "deleted": "#ff5555",
        "function": "#50fa7b",
        "heading": "#bd93f9",
        "inserted": "#50fa7b",
        "keyword": "#ff79c6",
        "number": "#bd93f9",
        "preprocessor": "#f8f8f2",
        "property": "#8be9fd",
        "regex": "#f1fa8c",
        "string": "#f1fa8c",
        "tag": "#ff79c6",
        "type": "#50fa7b",
      }
    `);
  });

  it('reads an uppercase hex as well as a lowercase one', () => {
    // Dracula's JSON is uppercase where GitHub's is lowercase, so a converter
    // that only matched lowercase would silently drop half of it.
    const upper = syntaxColorsFromTheme(dracula).syntax;
    const lower = syntaxColorsFromTheme({
      ...dracula,
      tokenColors: dracula.tokenColors?.map((entry) => ({
        ...entry,
        settings: {
          ...entry.settings,
          ...(entry.settings?.foreground
            ? { foreground: entry.settings.foreground.toLowerCase() }
            : {}),
        },
      })),
    } as VSCodeThemeJson).syntax;
    expect(upper).toEqual(lower);
  });

  it('matches a scope by its most specific prefix, as TextMate does', () => {
    // `comment` also covers `comment.line.double-slash`, and the narrowest
    // matching scope wins, which is the rule GitHub's own themes depend on when
    // their escape rules come after the broad string rule.
    const only = {
      tokenColors: [
        { scope: 'string', settings: { foreground: '#111111' } },
        {
          scope: 'string.regexp constant.character.escape',
          settings: { foreground: '#222222' },
        },
      ],
    } as VSCodeThemeJson;
    const converted = syntaxColorsFromTheme(only).syntax;
    expect(converted.string).toBe('#111111');
    // `regex` reads the narrow escape scope, which is the color the theme gives
    // it rather than the broad string one.
    expect(converted.regex).toBe('#222222');
  });

  it('colors numbers from a theme that names only constant.numeric', () => {
    // Most editor themes color numbers through `constant.numeric` rather than
    // the bare `constant` GitHub's use, so the representative scope has to be a
    // real number's scope for the prefix rule to reach either.
    const only = {
      tokenColors: [
        { scope: 'constant.numeric', settings: { foreground: '#111111' } },
      ],
    } as VSCodeThemeJson;
    expect(syntaxColorsFromTheme(only).syntax.number).toBe('#111111');
  });

  it('lets a later entry win for the same scope, as a theme rewrites a rule', () => {
    // Both GitHub themes restate a scope they already used, and the later color
    // is the one Shiki applies.
    const only = {
      tokenColors: [
        { scope: 'keyword', settings: { foreground: '#111111' } },
        { scope: 'keyword', settings: { foreground: '#222222' } },
      ],
    } as VSCodeThemeJson;
    expect(syntaxColorsFromTheme(only).syntax.keyword).toBe('#222222');
  });

  it('prefers an exact scope over a longer one that is not a prefix of it', () => {
    // `keyword.control` does not cover a `keyword` token, so the exact rule is
    // the one that applies; a converter that took the longest scope in the theme
    // rather than the longest prefix of the token's scope would read it wrong.
    const only = {
      tokenColors: [
        { scope: 'keyword', settings: { foreground: '#111111' } },
        { scope: 'keyword.control', settings: { foreground: '#222222' } },
      ],
    } as VSCodeThemeJson;
    expect(syntaxColorsFromTheme(only).syntax.keyword).toBe('#111111');
  });

  it('reads a string scope as one scope, even where it holds a space', () => {
    // Three entries in GitHub's own themes write two scopes in one string rather
    // than an array. Splitting on the space would give `string` the color meant
    // for the second scope, which recolors every literal in a code block.
    const only = {
      tokenColors: [
        { scope: 'string', settings: { foreground: '#111111' } },
        { scope: 'string variable', settings: { foreground: '#222222' } },
      ],
    } as unknown as VSCodeThemeJson;
    const converted = syntaxColorsFromTheme(only).syntax;
    expect(converted.string).toBe('#111111');
  });

  it('leaves a class the theme says nothing about out of the result', () => {
    // Out rather than defaulted, so the engine's own default still applies to it
    // and an imported theme does not blank a class it has no opinion about.
    const only = {
      tokenColors: [{ scope: 'keyword', settings: { foreground: '#111111' } }],
    } as VSCodeThemeJson;
    const converted = syntaxColorsFromTheme(only).syntax;
    expect(converted.keyword).toBe('#111111');
    expect(converted.string).toBeUndefined();
  });

  it('reads a theme with no token colors as nothing at all', () => {
    expect(syntaxColorsFromTheme({}).syntax).toEqual({});
    expect(syntaxColorsFromTheme({ tokenColors: [] }).syntax).toEqual({});
  });

  it('reads a theme whose entries set only a font style', () => {
    // GitHub's underline and strikethrough entries carry no foreground at all,
    // so an entry without one has to be skipped rather than read as transparent.
    const only = {
      tokenColors: [
        { scope: ['markup.underline'], settings: { fontStyle: 'underline' } },
        { scope: 'keyword', settings: { foreground: '#111111' } },
      ],
    } as VSCodeThemeJson;
    const converted = syntaxColorsFromTheme(only).syntax;
    expect(converted.keyword).toBe('#111111');
    expect(
      Object.values(converted).every((color) => color?.startsWith('#'))
    ).toBe(true);
  });

  it('ignores a colors object that has no editor keys', () => {
    expect(
      syntaxColorsFromTheme({ colors: { focusBorder: '#2188ff' } }).codeBlock
    ).toEqual({});
  });

  it('produces a light and a dark pair when an app zips two of them', () => {
    // The documented way to use a theme pair, and the one the presets and the
    // example screen follow.
    const light = syntaxColorsFromTheme(githubLight);
    const dark = syntaxColorsFromTheme(githubDark);
    const zipped: Record<string, { light: string; dark: string }> = {};
    for (const token of TOKEN_CLASSES) {
      const a = light.syntax[token];
      const b = dark.syntax[token];
      if (a && b) zipped[token] = { light: a, dark: b };
    }
    expect(zipped.keyword).toEqual({ light: '#cf222e', dark: '#ff7b72' });
    // The code block's own colors carry the two appearances of the code text,
    // which is what `plain` and `operator` draw in.
    expect(zipped.plain).toBeUndefined();
    expect(light.codeBlock.color).toBe('#1f2328');
    expect(dark.codeBlock.color).toBe('#e6edf3');
  });
});

/** An engine ARGB integer as the `#rrggbb` or `#rrggbbaa` a theme would write. */
function hex(argb: number): string {
  const value = (argb >>> 0).toString(16).padStart(8, '0');
  return `#${value.slice(2)}`;
}
