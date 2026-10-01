/**
 * `syntaxColorsFromTheme`: a VS Code or Shiki theme's JSON onto the engine's
 * token classes.
 *
 * Shiki already accepts a theme object, so Streamdown's `shikiTheme` objects can
 * be handed straight to this and their result put in `theme.syntax`. The theme
 * *names* Shiki bundles are not shipped, because names would mean bundling
 * Shiki's theme JSON; an app that wants one passes the object.
 *
 * This module is separate from `src/theme.ts` so an app that never imports it
 * does not bundle it.
 */
import { themeSlot } from './themeSlots';
import type { MarkdownTokenClass } from './types';
import type { VSCodeThemeJson, VSCodeTokenColor } from './vscodeTheme';

/**
 * One TextMate scope per token class, in preference order.
 *
 * The first scope a theme actually colors wins, matched by the most-specific-
 * prefix rule TextMate uses: a theme entry whose scope is a prefix of a
 * representative scope covers it. The scopes are chosen so that Shiki's
 * `github-light-default` and `github-dark-default` reproduce the engine's
 * `kTokenColors` exactly, and that comparison is the converter's own test.
 *
 * `plain` and `operator` have no scope on purpose. Both take the code block's
 * own text color, which is the theme's `editor.foreground` and reaches the block
 * through `codeBlock.color`. A theme's `variable.other` is its own idea of plain
 * text, and a theme is free to disagree with GitHub's about it, so reading one
 * here would replace a value the engine already gets right.
 */
const REPRESENTATIVE_SCOPES: Readonly<
  Record<MarkdownTokenClass, readonly string[]>
> = {
  plain: [],
  comment: ['comment', 'punctuation.definition.comment'],
  keyword: ['keyword', 'storage.type', 'storage'],
  type: ['entity.name.function', 'support.type'],
  string: ['string', 'punctuation.definition.string'],
  number: ['constant.numeric.decimal', 'support.constant', 'support'],
  constant: ['support.constant', 'constant'],
  regex: [
    'string.regexp constant.character.escape',
    'string.regexp',
    'source.regexp',
  ],
  function: ['entity.name.function'],
  property: ['meta.property-name', 'support.type.property-name'],
  operator: [],
  preprocessor: ['variable', 'entity.name'],
  tag: ['entity.name.tag', 'support.class.component'],
  attribute: ['meta.property-name', 'support.type.property-name'],
  inserted: ['markup.inserted', 'punctuation.definition.inserted'],
  deleted: ['markup.deleted', 'invalid.broken', 'message.error'],
  heading: ['markup.heading', 'markup.heading entity.name'],
};

/** The engine's token classes, in the order the theme's colors are returned. */
export const TOKEN_CLASSES: readonly MarkdownTokenClass[] = [
  'plain',
  'comment',
  'keyword',
  'type',
  'string',
  'number',
  'constant',
  'regex',
  'function',
  'property',
  'operator',
  'preprocessor',
  'tag',
  'attribute',
  'inserted',
  'deleted',
  'heading',
];

/**
 * A theme's `tokenColors` flattened to one color per scope, with a later entry
 * winning.
 *
 * A later entry wins because that is what TextMate does, and both GitHub themes
 * rely on it: their narrow escape rules come after the broad string rule.
 *
 * An array scope is a list of separate scopes the same rule applies to; a string
 * scope is one scope, even where it holds a space. Three entries in GitHub's own
 * themes do hold a space in a string scope, and splitting on it would give
 * `string` the color meant for an unrelated rule.
 *
 * Colors are lower-cased, because GitHub's themes write `#cf222e` and Dracula
 * writes `#FF79C6`, and a converted theme should not carry that difference into
 * the resolver or into a golden.
 */
function colorsByScope(
  entries: readonly VSCodeTokenColor[]
): ReadonlyMap<string, string> {
  const byScope = new Map<string, string>();
  for (const entry of entries) {
    const color = entry.settings?.foreground;
    if (typeof color !== 'string') continue;
    const normalized = color.toLowerCase();
    const scopes = Array.isArray(entry.scope) ? entry.scope : [entry.scope];
    for (const scope of scopes) {
      if (typeof scope === 'string' && scope !== '')
        byScope.set(scope, normalized);
    }
  }
  return byScope;
}

/** The color a theme gives a scope, by the most-specific-prefix rule. */
function colorForScope(
  byScope: ReadonlyMap<string, string>,
  scope: string
): string | undefined {
  const exact = byScope.get(scope);
  if (exact !== undefined) return exact;
  // A theme entry whose scope is a prefix of the representative one covers it,
  // so `comment` also colors `comment.line.double-slash`.
  let best: string | undefined;
  let bestLength = 0;
  for (const [candidate, color] of byScope) {
    if (!scope.startsWith(candidate)) continue;
    if (candidate.length <= bestLength) continue;
    bestLength = candidate.length;
    best = color;
  }
  return best;
}

export interface SyntaxColorsFromTheme {
  /** Token colors, ready for `theme.syntax`. */
  readonly syntax: Readonly<Partial<Record<MarkdownTokenClass, string>>>;
  /**
   * The code block's own colors, from the theme's `editor.background` and
   * `editor.foreground`, for `markdownStyle.codeBlock`.
   */
  readonly codeBlock: Readonly<{ backgroundColor?: string; color?: string }>;
}

/**
 * Reads a VS Code or Shiki theme's JSON.
 *
 * Call it twice and zip the results for a light and a dark pair:
 *
 * ```ts
 * const light = syntaxColorsFromTheme(githubLight);
 * const dark = syntaxColorsFromTheme(dracula);
 * theme={{ syntax: { ...light.syntax, ...dark.syntax } }}
 * ```
 *
 * A class the theme says nothing about is left out rather than defaulted here,
 * so the engine's own default still applies to it. A theme with no
 * `tokenColors`, or none this build can place, resolves to an empty object and
 * every code block keeps the palette it had.
 */
export function syntaxColorsFromTheme(
  theme: VSCodeThemeJson
): SyntaxColorsFromTheme {
  const syntax: Partial<Record<MarkdownTokenClass, string>> = {};
  const entries = Array.isArray(theme?.tokenColors) ? theme.tokenColors : [];
  if (entries.length > 0) {
    const byScope = colorsByScope(entries);
    for (const token of TOKEN_CLASSES) {
      for (const scope of REPRESENTATIVE_SCOPES[token]) {
        const color = colorForScope(byScope, scope);
        if (color !== undefined) {
          syntax[token] = color;
          break;
        }
      }
    }
  }
  const colors = theme?.colors;
  const codeBlock: { backgroundColor?: string; color?: string } = {};
  if (colors !== undefined && typeof colors === 'object') {
    const background = colors['editor.background'];
    if (typeof background === 'string') codeBlock.backgroundColor = background;
    const foreground = colors['editor.foreground'];
    if (typeof foreground === 'string') codeBlock.color = foreground;
  }
  return { syntax, codeBlock };
}

/**
 * The slot a token class writes to, or undefined for a class this build has no
 * slot for.
 *
 * Exported so a caller mapping a theme onto its own structures can check that a
 * class exists here before writing it, rather than sending an id native ignores.
 */
export function syntaxSlotName(token: MarkdownTokenClass): string | undefined {
  const name = `syntax.${token}`;
  return themeSlot(name) === undefined ? undefined : name;
}
