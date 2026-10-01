/**
 * Resolution of the `theme` and `markdownStyle` props.
 *
 * Two layers go in: coarse design tokens an app sets once for a whole chat, and
 * per-element styles for the cases a token is too broad for. Everything here is
 * pure, the way `resolveControls` is: the resolver turns both into a sparse list
 * of slot overrides, native interns that list into one immutable theme, and
 * nothing about a theme's shape crosses into JavaScript again.
 *
 * A no-theme view resolves to three empty arrays. That is what makes "no theme
 * means no change" structural rather than a special case: the native side sees
 * the same empty list an app would send, and its static default theme stands.
 */
import { processColor as rnProcessColor } from 'react-native';

import { themeSlot as slotFor } from './themeSlots';
import type { ThemeSlotName } from './themeSlots';
import type {
  EnrichedSyntaxKey,
  MarkdownColor,
  MarkdownStyle,
  MarkdownTheme,
  MarkdownTokenClass,
} from './types';

/** The three arrays the native `themeSlots`, `themeValues` and `themeFonts` props carry. */
export type ResolvedTheme = Readonly<{
  /** Slot ids, sorted ascending, one entry per override. */
  slots: readonly number[];
  /** One value per slot: a number, an ARGB color, or an index into `fonts`. */
  values: readonly number[];
  /** Font family names the font slots point at, by index. */
  fonts: readonly string[];
}>;

/**
 * One value, or values merged left to right the way a style array is.
 *
 * `undefined` is inside the union rather than only the parameter's, so a caller
 * can pass an optional prop straight through without narrowing it first.
 */
type Composable<T> =
  | T
  | undefined
  | ReadonlyArray<T | null | false | undefined>;

type Section = Readonly<Record<string, unknown>>;

const EMPTY: ResolvedTheme = { slots: [], values: [], fonts: [] };

// ------------------------------------------------------------------- warnings

/**
 * Development warnings, one per key rather than one per render.
 *
 * A value this version cannot draw is usually a typo or a config file that
 * drifted, and it is silent everywhere except here. Repeating it every render
 * would bury the first occurrence under a thousand copies of itself, so a key is
 * reported once and then remembered.
 */
const warned = new Set<string>();

function warnOnce(key: string, message: string): void {
  if (warned.has(key)) return;
  warned.add(key);
  console.warn(`[react-native-downstream] ${message}`);
}

/**
 * Forgets the warnings already printed. Exported for the tests, which run many
 * cases through one module and need each case able to warn.
 */
export function resetThemeWarnings(): void {
  warned.clear();
}

/** Whether anything has warned under `key`, so a test can assert one fired. */
export function hasWarned(key: string): boolean {
  return warned.has(key);
}

// Bound once at load rather than read through the import at each call.
//
// Both are called once per themeable slot on every resolution, so they sit in
// the hottest loop in this file. A bundler would inline them either way; reading
// them once keeps the cost here in the resolver rather than in whatever module
// interop the host happens to use.
const processColor = rnProcessColor as (value: unknown) => unknown;
const themeSlot = slotFor;

// ---------------------------------------------------------------------- colors

/** A color's ARGB per appearance; a missing half keeps that appearance's default. */
type ColorHalves = { light?: number; dark?: number };

/**
 * One color as the ARGB halves it names, or undefined when it cannot be drawn.
 *
 * `processColor` is React Native's own, so every color string `<Text>` accepts
 * works here too. A processed `DynamicColorIOS` arrives as
 * `{ dynamic: { light, dark } }` and becomes a pair. A `PlatformColor` arrives as
 * a semantic or resource-path object and is refused: Android resolves it to a
 * fixed integer when props are parsed, so it would not follow a configuration
 * change the way an unset default does.
 */
function resolveColor(value: unknown, origin: string): ColorHalves | undefined {
  if (value === undefined || value === null) return undefined;
  // A pair the app wrote itself. Either half alone is enough, and the half it
  // leaves out keeps the default for that appearance, so `{ dark: x }` themes
  // dark mode without freezing light mode to the same color.
  if (typeof value === 'object' && !Array.isArray(value)) {
    const pair = value as Partial<{ light: unknown; dark: unknown }>;
    if ('light' in pair || 'dark' in pair) {
      const light =
        pair.light == null
          ? undefined
          : resolveColor(pair.light, origin)?.light;
      const dark =
        pair.dark == null ? undefined : resolveColor(pair.dark, origin)?.dark;
      if (light === undefined && dark === undefined) return undefined;
      return { light, dark };
    }
  }
  const processed = processColor(value as never);
  if (processed === undefined) {
    warnOnce(
      origin,
      `${origin} is not a color this version can draw, so it is ignored.`
    );
    return undefined;
  }
  if (typeof processed === 'number') {
    return { light: processed, dark: processed };
  }
  const dynamic = (processed as { dynamic?: { light?: number; dark?: number } })
    .dynamic;
  if (dynamic === undefined) {
    warnOnce(
      origin,
      `${origin} is a PlatformColor, which this version does not accept because Android resolves it to a fixed integer and it could not follow an appearance change. Pass { light, dark } instead.`
    );
    return undefined;
  }
  const light = dynamic.light;
  const dark = dynamic.dark;
  if (typeof light !== 'number' || typeof dark !== 'number') {
    warnOnce(
      origin,
      `${origin} names both appearances of a color it does not have; give it a light and a dark value.`
    );
    return undefined;
  }
  return { light, dark };
}

// ------------------------------------------------------------- the slot writer

/**
 * Collects slot overrides and produces the three arrays.
 *
 * A color whose two appearances match emits one entry rather than two, which is
 * both smaller and what tells the platform the color does not change with
 * appearance. Ids are written into a map and sorted on the way out, so the order
 * a theme was written in never reaches the wire.
 */
class Slots {
  private readonly byId = new Map<number, number>();
  private readonly families: string[] = [];
  private readonly familyIndexes = new Map<string, number>();

  number(name: ThemeSlotName, value: number): void {
    const slot = themeSlot(name);
    if (slot !== undefined) this.byId.set(slot.id, value);
  }

  font(name: ThemeSlotName, family: string): void {
    const slot = themeSlot(name);
    if (slot === undefined || family === '') return;
    let index = this.familyIndexes.get(family);
    if (index === undefined) {
      index = this.families.length;
      this.families.push(family);
      this.familyIndexes.set(family, index);
    }
    this.byId.set(slot.id, index);
  }

  color(name: ThemeSlotName, color: ColorHalves): void {
    const slot = themeSlot(name);
    if (slot === undefined) return;
    // Each half the app named is sent on its own id and a half it left out is
    // not sent, which is how the engine tells `{ dark }` from one color in both
    // appearances. A later value for one half leaves the other as it was.
    if (color.light !== undefined) this.byId.set(slot.id, color.light);
    if (color.dark !== undefined && slot.darkId !== undefined) {
      // Addressed by the generated dark id, so a rename of the light half cannot
      // leave the pair sending one value.
      this.byId.set(slot.darkId, color.dark);
    }
  }

  resolve(): ResolvedTheme {
    if (this.byId.size === 0) return EMPTY;
    const ids = [...this.byId.keys()].sort((a, b) => a - b);
    return {
      slots: ids,
      values: ids.map((id) => this.byId.get(id) as number),
      fonts: this.families,
    };
  }
}

// ----------------------------------------------------------------- composition

/**
 * The entries of a composable prop, with the falsy ones dropped.
 *
 * `null`, `false` and `undefined` are skipped, so `[preset, isUser && {...}]` is
 * the shape the API documents and needs no merge helper.
 */
function entries<T>(value: Composable<T>): readonly T[] {
  if (value === undefined || value === null || value === false) return [];
  if (Array.isArray(value)) {
    return value.filter(
      (entry): entry is T =>
        entry !== null && entry !== undefined && entry !== false
    );
  }
  return [value as T];
}

function section(value: unknown): Section | undefined {
  return typeof value === 'object' && value !== null && !Array.isArray(value)
    ? (value as Section)
    : undefined;
}

// ------------------------------------------------------------- element styles

/**
 * One element's properties, each naming the slot it sets.
 *
 * The public property name is the key, so a value written for
 * `react-native-enriched-markdown` lands on the right slot without the resolver
 * having to guess from a slot name. An element this renderer does not draw has
 * no entry here at all, which is what makes it reportable.
 */
type ElementSlots = Readonly<Record<string, ThemeSlotName>>;

const TEXT_PROPERTIES = (prefix: string): ElementSlots => ({
  fontSize: `${prefix}.fontSize` as ThemeSlotName,
  fontFamily: `${prefix}.fontFamily` as ThemeSlotName,
  fontWeight: `${prefix}.fontWeight` as ThemeSlotName,
  lineHeight: `${prefix}.lineHeight` as ThemeSlotName,
  color: `${prefix}.color` as ThemeSlotName,
  marginTop: `${prefix}.marginTop` as ThemeSlotName,
  marginBottom: `${prefix}.marginBottom` as ThemeSlotName,
});

/** Six levels of the same properties, spelled out rather than computed. */
const HEADING_SLOTS: readonly ElementSlots[] = [
  TEXT_PROPERTIES('heading.1'),
  TEXT_PROPERTIES('heading.2'),
  TEXT_PROPERTIES('heading.3'),
  TEXT_PROPERTIES('heading.4'),
  TEXT_PROPERTIES('heading.5'),
  TEXT_PROPERTIES('heading.6'),
];

const ELEMENT_SLOTS: Readonly<Record<string, ElementSlots>> = {
  paragraph: {
    ...TEXT_PROPERTIES('paragraph'),
    fontStyle: 'paragraph.fontStyle',
  },
  // A heading sets `fontStyle` on its own level's slot, so the six are spelled
  // out rather than derived from the paragraph's.
  h1: { ...HEADING_SLOTS[0], fontStyle: 'heading.1.fontStyle' },
  h2: { ...HEADING_SLOTS[1], fontStyle: 'heading.2.fontStyle' },
  h3: { ...HEADING_SLOTS[2], fontStyle: 'heading.3.fontStyle' },
  h4: { ...HEADING_SLOTS[3], fontStyle: 'heading.4.fontStyle' },
  h5: { ...HEADING_SLOTS[4], fontStyle: 'heading.5.fontStyle' },
  h6: { ...HEADING_SLOTS[5], fontStyle: 'heading.6.fontStyle' },
  blockquote: {
    ...TEXT_PROPERTIES('blockquote'),
    fontStyle: 'blockquote.fontStyle',
    borderColor: 'blockquote.borderColor',
    borderWidth: 'blockquote.borderWidth',
    gapWidth: 'blockquote.gapWidth',
  },
  list: {
    ...TEXT_PROPERTIES('list'),
    markerColor: 'list.markerColor',
    bulletColor: 'list.bulletColor',
    marginLeft: 'list.marginLeft',
    gapWidth: 'list.gapWidth',
    itemSpacing: 'list.itemSpacing',
    markerFontSize: 'list.markerFontSize',
    markerFontFamily: 'list.markerFontFamily',
    markerFontWeight: 'list.markerFontWeight',
  },
  taskList: {
    checkedColor: 'taskList.checkedColor',
    borderColor: 'taskList.borderColor',
  },
  code: {
    fontFamily: 'code.fontFamily',
    fontSize: 'code.fontSize',
    color: 'code.color',
    backgroundColor: 'code.backgroundColor',
  },
  codeBlock: {
    ...TEXT_PROPERTIES('codeBlock'),
    backgroundColor: 'codeBlock.backgroundColor',
    borderColor: 'codeBlock.borderColor',
    borderWidth: 'codeBlock.borderWidth',
    borderRadius: 'codeBlock.borderRadius',
    padding: 'codeBlock.padding',
  },
  link: {
    color: 'link.color',
    underline: 'link.underline',
    fontFamily: 'link.fontFamily',
    fontWeight: 'link.fontWeight',
  },
  strong: {
    color: 'strong.color',
    fontFamily: 'strong.fontFamily',
    fontWeight: 'strong.fontWeight',
  },
  em: {
    color: 'em.color',
    fontFamily: 'em.fontFamily',
    fontStyle: 'em.fontStyle',
  },
  strikethrough: { color: 'strikethrough.color' },
  thematicBreak: {
    color: 'thematicBreak.color',
    height: 'thematicBreak.height',
    marginTop: 'thematicBreak.marginTop',
    marginBottom: 'thematicBreak.marginBottom',
  },
  table: {
    ...TEXT_PROPERTIES('table'),
    headerFontFamily: 'table.headerFontFamily',
    headerTextColor: 'table.headerTextColor',
    headerBackgroundColor: 'table.headerBackgroundColor',
    rowEvenBackgroundColor: 'table.rowEvenBackgroundColor',
    rowOddBackgroundColor: 'table.rowOddBackgroundColor',
    borderColor: 'table.borderColor',
    borderWidth: 'table.borderWidth',
    borderRadius: 'table.borderRadius',
    cellPaddingHorizontal: 'table.cellPaddingHorizontal',
    cellPaddingVertical: 'table.cellPaddingVertical',
    minColumnWidth: 'table.minColumnWidth',
    maxColumnWidth: 'table.maxColumnWidth',
  },
  footnote: { color: 'footnote.color' },
  imageAlt: { color: 'imageAlt.color' },
};

function applyProperty(
  slots: Slots,
  origin: string,
  name: ThemeSlotName,
  raw: unknown
): void {
  switch (typeof raw) {
    case 'number':
      if (name.endsWith('fontWeight')) {
        slots.number(name, weightOf(raw, origin));
      } else if (name.endsWith('fontStyle')) {
        slots.number(name, raw);
      } else {
        slots.number(name, raw);
      }
      return;
    case 'boolean':
      // Only `underline` is a flag, and `false` is as real a choice as `true`.
      slots.number(name, raw ? 1 : 0);
      return;
    case 'string':
      if (name.endsWith('fontFamily')) {
        slots.font(name, raw);
      } else if (name.endsWith('fontWeight')) {
        slots.number(name, weightOf(raw, origin));
      } else if (name.endsWith('fontStyle')) {
        // `italic` adds the trait; `normal` uses the face as it is, which is what
        // enriched documents for a custom family.
        slots.number(name, raw === 'italic' ? 1 : 0);
      } else {
        const color = resolveColor(raw, origin);
        if (color !== undefined) slots.color(name, color);
      }
      return;
    case 'object':
      // A `{ light, dark }` pair or a `DynamicColorIOS`, which is how an element
      // color follows the appearance. Only a color slot takes one.
      if (themeSlot(name)?.kind === 'color') setColor(slots, origin, raw, name);
      return;
    default:
      return;
  }
}

/** A CSS weight as the number the slot table holds. */
function weightOf(value: number | string, origin: string): number {
  if (typeof value === 'number') {
    if (value >= 100 && value <= 900) return value;
    warnOnce(
      origin,
      `${origin} has a font weight outside 100 to 900, so it is ignored.`
    );
    return 400;
  }
  if (value === 'normal' || value === 'regular') return 400;
  if (value === 'bold') return 700;
  const numeric = Number(value);
  if (Number.isFinite(numeric) && numeric >= 100 && numeric <= 900)
    return numeric;
  warnOnce(
    origin,
    `${origin} has a font weight this version does not have, so it is ignored.`
  );
  return 400;
}

/**
 * enriched-markdown's own syntax keys, mapped onto the engine's classes.
 *
 * `punctuation` is `operator` here, which is the class GitHub's themes give
 * punctuation anyway, and `variable` and `embedded` are `plain`, which is
 * enriched's own rule that they inherit the base color rather than recolor it.
 */
const ENRICHED_SYNTAX: Readonly<Record<EnrichedSyntaxKey, MarkdownTokenClass>> =
  {
    punctuation: 'operator',
    variable: 'plain',
    embedded: 'plain',
  };

function applySyntaxColors(
  slots: Slots,
  origin: string,
  syntax: Section
): void {
  for (const [key, value] of Object.entries(syntax)) {
    if (value === undefined) continue;
    const name = `syntax.${ENRICHED_SYNTAX[key as EnrichedSyntaxKey] ?? key}`;
    if (themeSlot(name) === undefined) {
      warnOnce(
        `${origin}.${key}`,
        `${origin}.${key} is not a token class this version has.`
      );
      continue;
    }
    const color = resolveColor(value, `${origin}.${key}`);
    if (color !== undefined) slots.color(name as ThemeSlotName, color);
  }
}

/**
 * Sets one color slot.
 *
 * `origin` is the path a warning names, and it is the caller's rather than the
 * slot's: a token that fans out over eight slots and cannot be drawn is one
 * mistake to report, not eight.
 */
function setColor(
  slots: Slots,
  origin: string,
  value: unknown,
  name: ThemeSlotName
): void {
  if (value === undefined) return;
  const color = resolveColor(value, origin);
  if (color !== undefined) slots.color(name, color);
}

/** The slots a color token fans out to, written beside the resolver. */
const TOKEN_COLORS: Readonly<Record<string, readonly ThemeSlotName[]>> = {
  // A native view has no inherited text color, so the body color is a token of
  // its own. An h6 is the one heading the token does not reach, because it is
  // already de-emphasized today.
  foreground: [
    'paragraph.color',
    'heading.1.color',
    'heading.2.color',
    'heading.3.color',
    'heading.4.color',
    'heading.5.color',
    'list.color',
    'table.color',
    'code.color',
    'codeBlock.color',
  ],
  mutedForeground: [
    'blockquote.color',
    'heading.6.color',
    'list.markerColor',
    'list.bulletColor',
    'imageAlt.color',
    'codeBlock.header.color',
    'codeBlock.header.buttonColor',
    'table.toolbar.buttonColor',
  ],
  primary: ['link.color', 'footnote.color', 'taskList.checkedColor'],
  muted: [
    'code.backgroundColor',
    'codeBlock.backgroundColor',
    'table.headerBackgroundColor',
  ],
  border: [
    'table.borderColor',
    'thematicBreak.color',
    'blockquote.borderColor',
    'codeBlock.borderColor',
  ],
  selection: ['selection.color'],
};

const ELEMENT_HEADER_SLOTS: Readonly<Record<string, ThemeSlotName>> = {
  color: 'codeBlock.header.color',
  buttonColor: 'codeBlock.header.buttonColor',
  backgroundColor: 'codeBlock.header.backgroundColor',
  borderColor: 'codeBlock.header.borderColor',
};

function applyElementStyle(slots: Slots, key: string, value: unknown): void {
  const origin = `markdownStyle.${key}`;
  const group = section(value);
  if (!group) return;

  // Keys with a nested group or its own namespace, handled before the flat table
  // so one pass over the properties does not have to know about them.
  if (key === 'admonition') {
    for (const [type, color] of Object.entries(group)) {
      const name = `admonition.${type}` as ThemeSlotName;
      if (themeSlot(name) === undefined) {
        warnOnce(
          `${origin}.${type}`,
          `${origin}.${type} is not an alert type this version has.`
        );
        continue;
      }
      setColor(slots, `${origin}.${type}`, color, name);
    }
    return;
  }
  if (key === 'codeBlock') {
    const header = section(group.header);
    if (header) {
      for (const [property, name] of Object.entries(ELEMENT_HEADER_SLOTS)) {
        setColor(slots, `${origin}.header.${property}`, header[property], name);
      }
    }
    const syntax = section(group.syntaxColors);
    if (syntax) applySyntaxColors(slots, `${origin}.syntaxColors`, syntax);
  }
  if (key === 'table') {
    const toolbar = section(group.toolbar);
    if (toolbar)
      setColor(
        slots,
        `${origin}.toolbar.buttonColor`,
        toolbar.buttonColor,
        'table.toolbar.buttonColor'
      );
  }

  const properties = ELEMENT_SLOTS[key];
  if (properties === undefined) {
    // Reported once per key rather than per property: a whole enriched style for
    // a feature this renderer does not draw is one thing to say.
    warnOnce(
      origin,
      `${origin} styles ${key}, which this renderer does not draw, so it is ignored.`
    );
    return;
  }
  for (const [property, name] of Object.entries(properties)) {
    const raw = group[property];
    if (raw === undefined) continue;
    applyProperty(slots, `${origin}.${property}`, name, raw);
  }
}

// ---------------------------------------------------------------- theme tokens

function applyThemeTokens(slots: Slots, theme: MarkdownTheme): void {
  const colors = section(theme.colors);
  if (colors) {
    for (const [token, value] of Object.entries(colors)) {
      if (value === undefined) continue;
      const names = TOKEN_COLORS[token];
      if (names === undefined) {
        warnOnce(
          `theme.colors.${token}`,
          `theme.colors.${token} is not a token this version has.`
        );
        continue;
      }
      // One origin for the whole fan-out, so an unreadable token is one warning
      // rather than one per slot it would have reached.
      for (const name of names)
        setColor(slots, `theme.colors.${token}`, value, name);
    }
  }
  if (typeof theme.radius === 'number') slots.number('radius', theme.radius);
  if (typeof theme.maxFontSizeMultiplier === 'number') {
    slots.number('maxFontSizeMultiplier', theme.maxFontSizeMultiplier);
  }
  const fonts = section(theme.fonts);
  if (fonts) {
    const sans = fonts.sans;
    if (typeof sans === 'string') {
      slots.font('paragraph.fontFamily', sans);
      // Body text is what a table cell and a list marker draw in, so the sans
      // family reaches them too rather than leaving the system font behind a
      // themed paragraph.
      slots.font('table.fontFamily', sans);
      slots.font('list.fontFamily', sans);
    }
    const heading = fonts.heading;
    if (typeof heading === 'string') {
      for (const level of HEADING_SLOTS) {
        slots.font(level.fontFamily as ThemeSlotName, heading);
      }
    }
    const mono = fonts.mono;
    if (typeof mono === 'string') {
      slots.font('code.fontFamily', mono);
      slots.font('codeBlock.fontFamily', mono);
    }
  }
  const syntax = section(theme.syntax);
  if (syntax) applySyntaxColors(slots, 'theme.syntax', syntax);
}

// -------------------------------------------------------------------- resolve

/**
 * Resolves `theme` and `markdownStyle` into the three arrays native reads.
 *
 * Both props accept one value or an array, merged left to right the way a React
 * Native style array is flattened, one level deeper: within an element or a
 * token group, a later entry overrides the properties it names and keeps the
 * rest. That is what makes a preset usable without a merge helper.
 *
 * The order is tokens, then element styles, so an element style wins over a token
 * and a token wins over the default: a token fans out over several slots and an
 * element names one of them. Only overrides are sent, so no theme is three empty
 * arrays.
 */
export function resolveTheme(
  theme?: Composable<MarkdownTheme>,
  markdownStyle?: Composable<MarkdownStyle>
): ResolvedTheme {
  const slots = new Slots();
  for (const value of entries<MarkdownTheme>(theme))
    applyThemeTokens(slots, value);
  for (const value of entries<MarkdownStyle>(markdownStyle)) {
    const style = section(value);
    if (!style) continue;
    for (const [key, element] of Object.entries(style)) {
      applyElementStyle(slots, key, element);
    }
  }
  return slots.resolve();
}

/**
 * Reads one color the way an app would, exposed for the tests and for a caller
 * that needs to check a value before handing it to the resolver. A value that
 * cannot be drawn warns once, exactly as it would inside a theme.
 */
export function readColor(
  value: MarkdownColor | undefined,
  origin = 'color'
): ColorHalves {
  return resolveColor(value, origin) ?? {};
}
