# Display behavior decisions

Decided 28 September 2026 for the native display path. Each entry lists the options, the evidence, and the choice.

## Soft line breaks display as line breaks

CommonMark renders a soft break as a newline in HTML, which a browser shows as a space. Chat renderers usually keep the author's line breaks instead.

- The Phase 0 fixture `unicode-crlf` expects `"…שלום\nNext line"` as visible text.
- Enriched Markdown 0.6.0, the benchmark baseline, turns `MD_TEXT_SOFTBR` into a line break node (`cpp/parser/MD4CParser.cpp`).
- GitHub issue and comment bodies, the closest GFM surface to assistant output, keep single newlines as breaks.
- Assistant output uses single newlines for addresses, verse, and short stacked lines. Joining them loses meaning, while keeping them never hides text.

**Choice:** keep soft breaks as line breaks. The oracle HTML is unchanged, because this is display projection, not parsing.

## md4c's full GitHub dialect, including admonitions and footnotes

md4c 0.6.0's `MD_DIALECT_GITHUB` adds admonitions (`> [!NOTE]`) and footnotes to tables, strikethrough, task lists, and permissive autolinks. The fixture oracle is `md2html --gfm`, which uses that dialect.

- Turning the extensions off would make the native parse diverge from the oracle. `> [!NOTE]` would become a quote with literal text, and `[^1]` would become literal brackets.
- GitHub renders both constructs, and models trained on GitHub content emit them.
- Enriched turns both off and parses raw HTML as text (`MD_FLAG_NOHTML`). That changes block structure, not only display: an HTML block containing `*a*` would render emphasis.

**Choice:** keep the oracle dialect. Admonitions render as a titled quote whose bar and title use the GitHub alert color. Footnote references are superscript numbers, and definitions follow a rule at the end. Raw HTML stays literal source text.

## Tables are separate native views with horizontal overflow

The Phase 0 matrix requires progressive rows after a valid separator, plus horizontal overflow, in the first release.

| Option | Column sizing | Horizontal overflow | Cost |
| --- | --- | --- | --- |
| Tab stops in the text layout | Fixed or width-dependent tab stops | None: long cells wrap into the next column | Already built; fails the contract |
| Grid drawn inside the text layout | Measured | Only if the whole view scrolls | Custom glyph layout per cell |
| Separate table view per table in a horizontal scroll view (Enriched's approach) | Measured per column, clamped | Yes | The document becomes a stack of segments |

**Choice:** a document is a vertical stack of segments. A text segment is a run of non-table blocks laid out as one text layout, as before. A table segment is one table in a horizontal scroll view.

- Column width is each column's widest cell, clamped to 56–280 points plus 12 points of horizontal padding. Rows are as tall as their tallest cell.
- Cells keep inline styles and links, and use the column alignment md4c reports.
- Measurement stacks segment heights with the same block spacing, so measured and mounted heights stay equal.
- Quote bars and list markers for a table inside a container are drawn in the table segment's margin.
- Segments also prepare for per-block identity: a later incremental tail can keep completed segments and their layout.

## Selection uses the platform text views

The first release requires selecting and copying visible text. Custom drawing views cannot provide selection handles, menus, or accessibility text navigation without reimplementing them.

- iOS text segments become non-editable `UITextView`s built on the same TextKit 1 stack and `DSLayoutManager`. Layout still matches standalone measurement, because the text container, padding, and insets are configured identically.
- Android text segments become `TextView`s configured to match the measuring `StaticLayout`: high-quality breaking, no hyphenation, no font padding, first-strong direction, and fallback line spacing.
- `selectable` toggles selection. Link taps keep working either way. Copy writes plain text without internal placeholders.
- Selection is per segment, as in Enriched: one drag cannot span a table.
