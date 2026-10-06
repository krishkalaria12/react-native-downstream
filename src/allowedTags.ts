/**
 * Resolution of the `allowedTags` prop.
 *
 * The shape is Streamdown's: `Record<tag, attributes[]>`. What leaves here is a
 * flat list of strings, because the engine reads a tag policy from a
 * `ParseOptions` and the only thing that crosses the bridge is a string array — a
 * codegen object prop would need a struct field per attribute and would give the
 * shadow node nothing cheaper to compare than the struct itself.
 *
 * Names are lowercased and attributes sorted, so `{ User: ['ID', 'name'] }` and
 * `{ user: ['name', 'id'] }` are one policy and a document parsed under one
 * reuses the parse under the other.
 */
import type { MarkdownAllowedTags } from './types';

/**
 * Opens a tag's entry in the wire form.
 *
 * The list is `-1,name,attr,attr,-1,name,…` rather than a plain run of strings
 * because the reader is a C++ loop that has to know where one tag's attributes
 * stop. A count per tag would need its own encoding and its own failure mode; a
 * sentinel is one string the parser never has to interpret.
 */
const TAG_START = '-1';

/**
 * The wire form, in `name` order.
 *
 * A tag with no attributes is two entries. The engine reads pairs by position
 * after each sentinel, which is what makes the order here the whole contract.
 */
export type ResolvedAllowedTags = Readonly<{ tags: readonly string[] }>;

function section(value: unknown): Record<string, unknown> | undefined {
  return typeof value === 'object' && value !== null
    ? (value as Record<string, unknown>)
    : undefined;
}

/**
 * The attributes a tag may carry, sorted and lowercased.
 *
 * `true` means no attributes, which is the safe reading: an app that allowed a
 * tag and named no attribute has not said which attributes it wants, and a
 * `style="…"` or `onclick="…"` reaching a text segment is exactly what a tag
 * allowlist exists to prevent.
 */
function attributesOf(value: unknown): string[] {
  if (value === true || value === undefined) return [];
  if (!Array.isArray(value)) return [];
  return value
    .filter((entry): entry is string => typeof entry === 'string')
    .map((entry) => entry.trim().toLowerCase())
    .filter((entry) => entry !== '')
    .sort();
}

/**
 * Resolves `allowedTags` into the flat list the engine reads.
 *
 * A name that is not a string, an empty one, and a value that is neither `true`
 * nor an array are all dropped rather than throwing: the prop comes from JSON as
 * often as from a literal, and an app that misconfigured one tag should still get
 * every other one.
 */
export function resolveAllowedTags(
  allowedTags: MarkdownAllowedTags | undefined
): ResolvedAllowedTags {
  const source = section(allowedTags);
  if (!source) return { tags: [] };
  const flat: string[] = [];
  for (const name of Object.keys(source).sort()) {
    const tag = name.trim().toLowerCase();
    if (tag === '') continue;
    flat.push(TAG_START, tag, ...attributesOf(source[name]));
  }
  return { tags: flat };
}

/** True when the prop named no tag, which is the default. */
export function hasAllowedTags(resolved: ResolvedAllowedTags): boolean {
  return resolved.tags.length > 0;
}
