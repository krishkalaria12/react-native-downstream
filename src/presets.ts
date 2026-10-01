/**
 * The presets the package ships.
 *
 * A preset is a plain frozen `{ theme, markdownStyle }` value with no behavior
 * of its own: it goes through the same resolver as any app value, so it costs a
 * few hundred bytes and nothing at run time until it is used. Both props accept
 * an array, so an app starts from a preset and overrides a few values without a
 * merge helper:
 *
 * ```tsx
 * <DownstreamMarkdown theme={[presets.streamdown.theme, { radius: 12 }]} />
 * ```
 *
 * These values are public API. Streamdown or Primer changing a color does not
 * change ours silently: updating a preset is a deliberate change, listed in the
 * changelog, and the snapshot test makes it a visible diff.
 */
import type { MarkdownPreset } from './types';

/** Freeze both halves, so a spread of a preset cannot mutate the shared value. */
function preset(value: MarkdownPreset): MarkdownPreset {
  return Object.freeze({
    theme: Object.freeze(value.theme),
    markdownStyle: Object.freeze(value.markdownStyle),
  });
}

/**
 * GitHub's Markdown look, with concrete Primer colors instead of the platform's
 * semantic ones.
 *
 * The default already follows GitHub's code themes for tokens; what this preset
 * adds is that the prose colors are concrete too, so iOS and Android draw the
 * same values rather than each drawing its own `label` color. Use it when a
 * design calls for GitHub's look on both platforms at once.
 */
const github: MarkdownPreset = preset({
  theme: {
    // Primer's canvas, accent, subtle canvas, and border, light and dark. The
    // source is Primer's color foundations, which is what GitHub's own themes
    // resolve --foreground, --accent-fg, --canvas-subtle and --border to.
    colors: {
      foreground: { light: '#1f2328', dark: '#e6edf3' },
      primary: { light: '#0969da', dark: '#4493f8' },
      muted: { light: '#f6f8fa', dark: '#161b22' },
      border: { light: '#d0d7de', dark: '#30363d' },
    },
    // GitHub's own content radius for a code card and a table.
    radius: 6,
    // Tokens are left at the default, which is already GitHub's Primer
    // "prettylights" palette for light and dark.
  },
  markdownStyle: {},
});

/**
 * Streamdown's default look, for an app moving a chat UI from the web.
 *
 * Colors are the shadcn default variables from Streamdown's `styling.mdx`,
 * converted from the `H S% L%` triples that document writes to hex. Sizes and
 * spacing are the Tailwind classes in Streamdown's `lib/components.tsx` and
 * `lib/code-block/*`, converted at the 16-pixel root Tailwind assumes, so a
 * reader sees what the web component shows rather than an approximation.
 *
 * Streamdown at `vercel/streamdown` commit `08da224` (2.7.0).
 */
const streamdown: MarkdownPreset = preset({
  theme: {
    colors: {
      // --foreground, --primary, --muted, --muted-foreground and --border, in
      // the document's own `:root` and `.dark` blocks.
      foreground: { light: '#020817', dark: '#f8fafc' },
      primary: { light: '#0f172a', dark: '#f8fafc' },
      muted: { light: '#f1f5f9', dark: '#1e293b' },
      mutedForeground: { light: '#64748b', dark: '#94a3b8' },
      border: { light: '#e2e8f0', dark: '#1e293b' },
    },
    // --radius: 0.5rem at a 16-pixel root.
    radius: 8,
  },
  markdownStyle: {
    paragraph: {
      // No class of its own on a paragraph: body text inherits the root size.
      fontSize: 16,
    },
    // `mt-6 mb-2 font-semibold` with text-3xl, 2xl, xl, lg, base and sm.
    h1: { fontSize: 30, fontWeight: '600', marginTop: 24, marginBottom: 8 },
    h2: { fontSize: 24, fontWeight: '600', marginTop: 24, marginBottom: 8 },
    h3: { fontSize: 20, fontWeight: '600', marginTop: 24, marginBottom: 8 },
    h4: { fontSize: 18, fontWeight: '600', marginTop: 24, marginBottom: 8 },
    h5: { fontSize: 16, fontWeight: '600', marginTop: 24, marginBottom: 8 },
    h6: { fontSize: 14, fontWeight: '600', marginTop: 24, marginBottom: 8 },
    blockquote: {
      // `my-4 border-l-4 pl-4 text-muted-foreground italic`, with the bar at a
      // tenth of --muted-foreground, which is what /30 means against it.
      marginTop: 16,
      marginBottom: 16,
      borderWidth: 4,
      borderColor: { light: '#a3a3a3', dark: '#6b7280' },
      gapWidth: 16,
      color: { light: '#64748b', dark: '#94a3b8' },
      fontStyle: 'italic',
    },
    list: {
      // `list-inside [li_&]:pl-6` for the marker column and `py-1` for the
      // space between items.
      marginLeft: 24,
      itemSpacing: 4,
      gapWidth: 8,
      markerColor: { light: '#64748b', dark: '#94a3b8' },
    },
    code: {
      // `rounded bg-muted px-1.5 py-0.5 font-mono text-sm`.
      fontSize: 14,
      backgroundColor: { light: '#f1f5f9', dark: '#1e293b' },
    },
    codeBlock: {
      // The body is `rounded-md border border-border bg-background p-4 text-sm`
      // inside a `rounded-xl border border-border p-2` card, so the card's own
      // padding is 8 and the body's 16 sits inside it.
      fontSize: 14,
      padding: 24,
      borderRadius: 12,
      borderWidth: 1,
      borderColor: { light: '#e2e8f0', dark: '#1e293b' },
      backgroundColor: { light: '#ffffff', dark: '#020817' },
      color: { light: '#1f2328', dark: '#e6edf3' },
      marginTop: 16,
      marginBottom: 16,
      header: {
        // The header is `flex h-8 items-center text-muted-foreground text-xs`,
        // and its buttons sit on a sidebar-tinted chip.
        color: { light: '#64748b', dark: '#94a3b8' },
        buttonColor: { light: '#64748b', dark: '#94a3b8' },
      },
    },
    link: {
      // `font-medium text-primary underline`.
      color: { light: '#0f172a', dark: '#f8fafc' },
      underline: true,
      fontWeight: '500',
    },
    strong: { fontWeight: 600 },
    em: { fontStyle: 'italic' },
    strikethrough: { color: { light: '#64748b', dark: '#94a3b8' } },
    thematicBreak: {
      // `my-6 border-border`, where a rule is one point and its space is 24.
      color: { light: '#e2e8f0', dark: '#1e293b' },
      height: 1,
      marginTop: 24,
      marginBottom: 24,
    },
    table: {
      // `px-4 py-2` on every cell and `bg-muted/80` on the header row.
      cellPaddingHorizontal: 16,
      cellPaddingVertical: 8,
      borderColor: { light: '#e2e8f0', dark: '#1e293b' },
      borderWidth: 1,
      headerBackgroundColor: { light: '#f1f5f9', dark: '#1e293b' },
      toolbar: { buttonColor: { light: '#64748b', dark: '#94a3b8' } },
    },
  },
});

export const presets: Readonly<{
  github: MarkdownPreset;
  streamdown: MarkdownPreset;
}> = Object.freeze({ github, streamdown });
