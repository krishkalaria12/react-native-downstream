#!/usr/bin/env node
// Writes the two generated halves of the theme slot table:
//
//     node src/cpp/theme/generate-theme-slots.mjs
//
//   src/cpp/theme/ThemeSlots.h   the C++ view: enum, roles, defaults, layout bits
//   src/themeSlots.ts            the TypeScript view: name to id, kind, layout
//
// No build runs this: both outputs are checked in, and `pnpm test:native` fails
// if regenerating changes either one, so a slot cannot drift between the two
// languages the way the metric constants in DSRenderedDocument.mm and
// RenderedDocument.kt already had.
//
// src/theme/theme-slots.json next to this script is the single source: every
// themeable value, its stable id, its kind, whether it changes layout, and its
// default. The ids are append-only, and this script refuses to write a table
// that reuses or reorders one, because a theme built against one release has to
// keep meaning the same thing in the next.

import { readFileSync, writeFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));
const source = resolve(join(HERE, '..', '..', 'theme', 'theme-slots.json'));

// Where the three generated files land. The environment override exists so the
// test can regenerate into a scratch directory and compare against what is
// checked in, rather than overwriting the tree it is checking.
const OUT = process.env.DS_THEME_SLOT_OUT ? resolve(process.env.DS_THEME_SLOT_OUT) : null;
const GENERATED = {
  'ThemeSlots.h': resolve(join(HERE, 'ThemeSlots.h')),
  'themeSlots.ts': resolve(join(HERE, '..', '..', 'themeSlots.ts')),
  'ThemeSlots.kt': resolve(
    join(HERE, '..', '..', 'android', 'src', 'main', 'java', 'com', 'downstream', 'ThemeSlots.kt'),
  ),
};
const out = (name) => (OUT ? join(OUT, name) : GENERATED[name]);
const headerOut = out('ThemeSlots.h');
const typesOut = out('themeSlots.ts');
const kotlinOut = out('ThemeSlots.kt');

const arg = (name, fallback) => {
  const index = process.argv.indexOf(`--${name}`);
  return index >= 0 && process.argv[index + 1] ? process.argv[index + 1] : fallback;
};

const table = JSON.parse(readFileSync(arg('in', source), 'utf8'));
const slots = table.slots;
const roles = table.roles;

if (!Array.isArray(slots) || slots.length === 0) {
  console.error('refusing to write: no slots');
  process.exit(1);
}

const KINDS = new Set(['metric', 'color', 'font', 'weight', 'flag']);
const roleIndex = new Map(roles.map((role, index) => [role, index]));

// TokenClass::Count is 17 in Document.h. The generated header offsets a token
// class by kRoleTokenClassBase, which is fixed at 32 here, so both namespaces
// have to fit under it for the two never to be read as each other.
const TOKEN_CLASS_BASE = 32;
const TOKEN_CLASS_COUNT = 17;
if (roles.length >= TOKEN_CLASS_BASE || TOKEN_CLASS_COUNT > TOKEN_CLASS_BASE) {
  console.error(
    `refusing to write: ${roles.length} roles and ${TOKEN_CLASS_COUNT} token classes do not both fit under ${TOKEN_CLASS_BASE}`
  );
  process.exit(1);
}

// A color slot takes two ids: the light value and the dark one. A color that is
// the same in both appearances sends one entry and the platform uses it for both;
// a pair sends the light id and the one after it. The table lists only the light
// id, so the id after a color is reserved, which is what keeps a pair's two ids
// adjacent and every id distinct.
const expanded = [];
for (const slot of slots) {
  expanded.push({ ...slot, appearance: 'light' });
  if (slot.kind === 'color') {
    expanded.push({ ...slot, id: slot.id + 1, appearance: 'dark' });
  }
}

const seenIds = new Map();
const seenNames = new Map();
let previousId = 0;

for (const slot of expanded) {
  if (!KINDS.has(slot.kind)) {
    console.error(`refusing to write: ${slot.name} has unknown kind ${slot.kind}`);
    process.exit(1);
  }
  if (!Number.isInteger(slot.id) || slot.id <= 0) {
    console.error(`refusing to write: ${slot.name} has a non-positive id`);
    process.exit(1);
  }
  // Append-only. A reorder or a reuse would silently repoint every theme an app
  // has already written, so it is a hard failure rather than a warning.
  if (seenIds.has(slot.id)) {
    console.error(
      `refusing to write: id ${slot.id} is used by both ${seenIds.get(slot.id)} and ${slot.name}`
    );
    process.exit(1);
  }
  if (slot.id < previousId) {
    console.error(
      `refusing to write: ${slot.name} (${slot.id}) comes after id ${previousId}; ids are append-only`
    );
    process.exit(1);
  }
  // A color's two ids share its name on purpose, so only the light half is
  // checked: the table lists a slot once and the generator expands it.
  if (slot.appearance === 'light' && seenNames.has(slot.name)) {
    console.error(`refusing to write: ${slot.name} is listed twice`);
    process.exit(1);
  }
  if (!/^[a-z][A-Za-z0-9]*(\.[A-Za-z0-9]+)*$/.test(slot.name)) {
    console.error(`refusing to write: ${slot.name} is not a slot name`);
    process.exit(1);
  }
  if (slot.appearance === 'dark' && slot.kind !== 'color') {
    console.error(`refusing to write: ${slot.name} is a second appearance of a non-color`);
    process.exit(1);
  }
  if (slot.kind === 'font') {
    // A font has no meaningful zero, so its default is the absent family and the
    // platforms fall back to the system font, as <Text> does.
    if (slot.default !== undefined && slot.default !== '') {
      console.error(`refusing to write: font slot ${slot.name} has a default`);
      process.exit(1);
    }
  } else if (typeof slot.default !== 'number' && typeof slot.default !== 'string') {
    console.error(`refusing to write: ${slot.name} has no default`);
    process.exit(1);
  }
  if (slot.kind === 'color' && typeof slot.default === 'string' && !slot.default.startsWith('tokens:')) {
    if (!roleIndex.has(slot.default)) {
      console.error(`refusing to write: ${slot.name} names unknown default ${slot.default}`);
      process.exit(1);
    }
  }
  seenIds.set(slot.id, slot.name);
  seenNames.set(slot.name, slot.id);
  previousId = slot.id;
}

// ------------------------------------------------------------------- naming

const cap = (text) => text[0].toUpperCase() + text.slice(1);
const cString = (text) => `"${String(text).replace(/[\\"]/g, '\\$&')}"`;
const tsString = (text) => `'${String(text).replace(/'/g, "\\'")}'`;
/** A Kotlin constant name: paragraph.fontSize becomes SLOT_PARAGRAPH_FONT_SIZE. */
const kotlinName = (name) =>
  'SLOT_' + name.replace(/\.dark$/, '').split('.').join('_').replace(/([a-z0-9])([A-Z])/g, '$1_$2').toUpperCase();
const enumName = (name) =>
  'kSlot' + name.split('.').map((part) => cap(part)).join('');
const roleEnum = (name) => 'kRole' + cap(name);

/**
 * A color default as a C++ expression. A token color names a \`TokenClass\`
 * offset by \`kRoleTokenClassBase\` rather than a role, so one uint16 holds
 * both and the two namespaces cannot collide.
 */
function colorDefault(slot) {
  if (slot.default.startsWith('tokens:')) {
    return `kRoleTokenClassBase + static_cast<uint16_t>(TokenClass::${slot.default.slice('tokens:'.length)})`;
  }
  return `static_cast<uint16_t>(ColorRole::${roleEnum(slot.default)})`;
}

/** A metric, weight or flag default as a C++ double literal. */
function numericDefault(slot) {
  const value = String(slot.default);
  return value.includes('.') ? value : `${value}.0`;
}

const KINDS_CPP = {
  metric: 'Metric',
  color: 'Color',
  font: 'Font',
  weight: 'Weight',
  flag: 'Flag',
};

const byName = new Map(slots.map((slot) => [slot.name, slot]));

/**
 * The enum name for a slot the table must have, or a hard failure.
 *
 * A color's name is the light one; the dark value is the id after it and shares
 * the enum, so a platform reads `kSlotLinkColor` and `kSlotLinkColor + 1`.
 */
function required(name) {
  const slot = slotNamed(name);
  return enumName(slot.name);
}

/** The slot named `name`, refusing to write a table that does not have it. */
function slotNamed(name) {
  const slot = byName.get(name);
  if (!slot) {
    console.error(`refusing to write: the table has no slot named ${name}`);
    process.exit(1);
  }
  return slot;
}

/**
 * The six heading slots for one property, in level order.
 *
 * The heading levels are spelled out here rather than assumed to be eight slots
 * apart, because a level gaining or losing a property would then silently
 * repoint every level above it.
 */
function heading(property) {
  const ids = [];
  for (let level = 1; level <= 6; level++) {
    ids.push(required(`heading.${level}.${property}`));
  }
  return ids.join(', ');
}

// ------------------------------------------------------------------- C++ side

const header = `// GENERATED FILE. Written by src/cpp/theme/generate-theme-slots.mjs.
//
//   node src/cpp/theme/generate-theme-slots.mjs
//
// One row per themeable value the renderers draw with, from
// src/theme/theme-slots.json. The ids are append-only: JavaScript sends a slot
// id, this file indexes the theme's arrays with it, and a slot a later feature
// adds is a new line. Nothing here is edited by hand.

#pragma once

#include "Document.h"

#include <cstddef>
#include <cstdint>

namespace downstream::theme {

/// What a slot holds, and therefore how a value travels to it.
enum class SlotKind : uint8_t {
  /// A number in points before font scaling.
  Metric = 0,
  /// An ARGB light and dark pair, or a role while it is unset.
  Color = 1,
  /// An index into the theme's font name list.
  Font = 2,
  /// A CSS font weight, 100 to 900.
  Weight = 3,
  /// Zero or one.
  Flag = 4,
};

/// The platform color a slot falls back to while nothing overrides it. A role
/// rather than an integer on purpose: \`label\` is \`UIColor.labelColor\` on iOS
/// and the existing Palette value on Android, so an unset color keeps following
/// increased contrast and appearance, which a flattened integer could not.
/// \`inherit\` means the platform draws nothing and the enclosing block's color
/// shows through.
enum class ColorRole : uint16_t {
${roles.map((role) => `  ${roleEnum(role)} = ${roleIndex.get(role)},`).join('\n')}
};

/// A color slot's default is either a \`ColorRole\` or a \`TokenClass\`, both
/// uint16, so one field holds both. The two namespaces are offset rather than
/// interleaved: a role is below this and a class is this plus the class, so no
/// role can ever be read as a class. \`TokenClass::Count\` is 17 and this table
/// has fewer roles than that, so the base clears both.
constexpr uint16_t kRoleTokenClassBase = ${TOKEN_CLASS_BASE};

enum Slot : uint32_t {
${slots.map((slot) => `  ${enumName(slot.name)} = ${slot.id},`).join('\n')}
};

/// One past the highest id in use. A theme naming an id at or above this is a
/// build against a newer slot table, and every platform ignores it.
constexpr uint32_t kSlotIdCount = ${previousId + 1};

/// The id holding the dark half of a color, which is always the one after the
/// light half. Zero for any other slot, and for the dark half itself. Indexed
/// by id - 1, so the pair's second id is one more than the first.
constexpr uint32_t kSlotDark[] = {${expanded
  .map((slot) => (slot.appearance === 'dark' ? '0' : slot.kind === 'color' ? String(slot.id + 1) : '0'))
  .join(', ')}};

/// The id holding the light half of a color, which is always the one before the
/// dark half. Zero for any other slot. Indexed by id - 1. A color's two halves
/// are one resolved value, so reading either id returns it, and this is what
/// makes that true rather than a platform having to know which half it holds.
constexpr uint32_t kSlotLight[] = {${expanded
  .map((slot) => (slot.appearance === 'dark' ? String(slot.id - 1) : '0'))
  .join(', ')}};

/// Whether each id changes size. These slots alone form the layout key, so a
/// color-only change never invalidates a measurement. Indexed by id - 1.
constexpr uint8_t kSlotLayout[] = {${expanded.map((slot) => (slot.layout ? '1' : '0')).join(', ')}};

/// The kind of each id. Indexed by id - 1.
constexpr uint8_t kSlotKinds[] = {${expanded
  .map((slot) => `static_cast<uint8_t>(SlotKind::${KINDS_CPP[slot.kind]})`)
  .join(', ')}};

/// The default of a non-color slot as a double: a metric, a weight, or a flag's
/// 0 or 1. Zero for a color or a font, whose defaults are below. Indexed by
/// id - 1.
constexpr double kSlotDefaults[] = {${expanded
  .map((slot) => (slot.kind === 'color' || slot.kind === 'font' ? '0' : numericDefault(slot)))
  .join(', ')}};

/// The default of a color slot: a \`ColorRole\`, or \`kRoleTokenClassBase\` plus
/// a \`TokenClass\` for a syntax color. Zero for any other slot. The dark half
/// of a pair carries the same default, so a platform reads one entry either way.
/// Indexed by id - 1.
constexpr uint16_t kSlotColorDefaults[] = {${expanded
  .map((slot) => (slot.kind === 'color' ? colorDefault(slot) : '0'))
  .join(', ')}};

/// The slot name of each id, for a development warning that names the value.
/// The dark half of a pair names the same slot, so a warning about either half
/// reads the way the theme was written. Indexed by id - 1.
constexpr const char *kSlotNames[] = {
${expanded
  .map((slot) => `    ${cString(slot.appearance === 'dark' ? slot.name.replace(/\\.dark$/, '') : slot.name)},`)
  .join('\n')}
};

/// One slot per heading level and property, so \`blockFont\` reads a level's
/// metrics without arithmetic on slot ids. Level n is at index n - 1.
namespace heading {

constexpr uint32_t kFontSize[6] = {${heading('fontSize')}};
constexpr uint32_t kFontWeight[6] = {${heading('fontWeight')}};
constexpr uint32_t kFontFamily[6] = {${heading('fontFamily')}};
constexpr uint32_t kLineHeight[6] = {${heading('lineHeight')}};
constexpr uint32_t kColor[6] = {${heading('color')}};
constexpr uint32_t kFontStyle[6] = {${heading('fontStyle')}};
constexpr uint32_t kMarginTop[6] = {${heading('marginTop')}};
constexpr uint32_t kMarginBottom[6] = {${heading('marginBottom')}};

} // namespace heading

/// The syntax slot per \`TokenClass\`, so a token run's class picks its color
/// without a switch. Indexed by the class. A pair's light and dark halves follow
/// the base slot, so a platform reads \`kSyntax[token]\` and \`+ 1\`.
constexpr uint32_t kSyntax[static_cast<size_t>(TokenClass::Count)] = {
${expanded
  .filter((slot) => slot.appearance === 'light' && slot.name.startsWith('syntax.'))
  .map((slot) => `    ${enumName(slot.name)},`)
  .join('\n')}
};

} // namespace downstream::theme
`;

writeFileSync(headerOut, header);

// -------------------------------------------------------------- TypeScript side

const slotKind = (slot) => (slot.kind === 'metric' ? "'metric'" : `'${slot.kind}'`);

const types = `// GENERATED FILE. Written by src/cpp/theme/generate-theme-slots.mjs.
//
//   node src/cpp/theme/generate-theme-slots.mjs
//
// The TypeScript half of the theme slot table. The ids are the ones C++ indexes
// with, so the resolver never writes a name native cannot read, and a value
// written for one release keeps its meaning in the next.

/** What a slot holds. Mirrors \`downstream::theme::SlotKind\`. */
export type ThemeSlotKind = 'metric' | 'color' | 'font' | 'weight' | 'flag';

export interface ThemeSlot {
  /** Stable and append-only. The first of a color's two ids. */
  readonly id: number;
  readonly kind: ThemeSlotKind;
  /** True when changing the slot changes size rather than only appearance. */
  readonly layout: boolean;
  /**
   * The id holding the dark half of the color, or undefined for anything else.
   * A color that is the same in both appearances sends only \`id\` and the
   * platform uses it for both.
   */
  readonly darkId?: number;
}

/**
 * Every themeable value, by name. Names are dotted rather than nested because
 * the wire format is positional and a rename would be a wire change.
 *
 * Kept narrow internally so \`ThemeSlotName\` below is the union of the real names,
 * then widened on the way out: a caller iterating the table wants to read
 * \`darkId\` off any entry, which a per-entry literal type would refuse.
 */
const SLOTS = {
${slots
  .map(
    (slot) =>
      `  ${tsString(slot.name)}: { id: ${slot.id}, kind: ${slotKind(slot)}, layout: ${slot.layout}${
        slot.kind === 'color' ? `, darkId: ${slot.id + 1}` : ''
      } },`
  )
  .join('\n')}
} as const satisfies Readonly<Record<string, ThemeSlot>>;

export const THEME_SLOTS: Readonly<Record<string, ThemeSlot>> = SLOTS;

export type ThemeSlotName = keyof typeof SLOTS;

/** The slot names in id order. Ascending, so a resolved theme sorts by id. */
export const THEME_SLOT_NAMES = [
${slots.map((slot) => `  ${tsString(slot.name)},`).join('\n')}
] as const satisfies readonly ThemeSlotName[];

/** One past the highest id in use. */
export const THEME_SLOT_COUNT = ${previousId + 1};

const BY_NAME: ReadonlyMap<string, ThemeSlot> = new Map<string, ThemeSlot>(
  (Object.keys(SLOTS) as ThemeSlotName[]).map((name) => [name, SLOTS[name]])
);

/**
 * The slot for \`name\`, or undefined for a name this build does not have. An
 * unknown name is one JavaScript cannot send, because the generated table is the
 * only source of ids.
 */
export function themeSlot(name: string): ThemeSlot | undefined {
  return BY_NAME.get(name);
}
`;

writeFileSync(typesOut, types);

// ------------------------------------------------------------- Kotlin side

// The slot ids for the Android renderer, written rather than spelled out: a
// renderer reading its ids from a hand-kept list would be one more copy of the
// table to drift, and adding a slot in one place and forgetting the other
// compiles and then draws the wrong color.

// A heading's slots as one array per property, matching the engine's
// `heading::` namespace: level n is at index n - 1.
const HEADING_PROPERTIES = [
  ['FONT_SIZE', 'fontSize'],
  ['FONT_WEIGHT', 'fontWeight'],
  ['FONT_FAMILY', 'fontFamily'],
  ['LINE_HEIGHT', 'lineHeight'],
  ['COLOR', 'color'],
  ['FONT_STYLE', 'fontStyle'],
  ['MARGIN_TOP', 'marginTop'],
  ['MARGIN_BOTTOM', 'marginBottom'],
];

const kotlinHeading =
  '\n\n  /**\n' +
  '   * A heading level\'s slots, one array per property, in level order. Level n\'s\n' +
  '   * slot is at index n - 1, as in the engine\'s `heading::` namespace.\n' +
  '   */\n' +
  '  object Heading {\n' +
  HEADING_PROPERTIES.map(
    ([constant, property]) =>
      `    /** \`heading.N.${property}\`, for levels one to six. */\n` +
      `    val ${constant} = intArrayOf(${[1, 2, 3, 4, 5, 6]
        .map((level) => slotNamed(`heading.${level}.${property}`).id)
        .join(', ')})`,
  ).join('\n') +
  '\n  }';

/** One `const val` per id, in id order. */
const kotlinSlots = expanded
  .map((slot) => {
    const name = slot.appearance === 'dark' ? slot.name.replace(/\\.dark$/, '') : slot.name;
    const suffix = slot.appearance === 'dark' ? ', dark half' : '';
    // The dark half of a color is its own id, so it needs its own name: both are
    // in this object and two constants with one name would not compile.
    const constant = kotlinName(slot.name) + (slot.appearance === 'dark' ? '_DARK' : '');
    return '  /** ' + cString(name) + suffix + ' */\n  const val ' + constant + ' = ' + slot.id;
  })
  .join('\n');

const kotlin = `// GENERATED FILE. Written by src/cpp/theme/generate-theme-slots.mjs.
//
//   node src/cpp/theme/generate-theme-slots.mjs
//
// The theme slot ids for the Android renderer, from the same table C++ and
// JavaScript read. Object rather than consts, so an id named at a use site reads
// as the slot's name rather than as a number.

package com.downstream

/** One themeable value's id. A color's dark half is its id plus one. */
internal object ThemeSlots {
${kotlinSlots}

  /** One past the highest id in use. */
  const val SLOT_ID_COUNT = ${previousId + 1}
${kotlinHeading}
}
`;

writeFileSync(kotlinOut, kotlin);

console.error(
  OUT
    ? `wrote ${slots.length} slots (${expanded.length} ids) to ${OUT} (highest id ${previousId})`
    : `wrote ${slots.length} slots (${expanded.length} ids) to ${headerOut}, ${typesOut} and ${kotlinOut} (highest id ${previousId})`
);