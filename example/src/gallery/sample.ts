/** Complete document exercising every first-release block and inline form. */
export const featureSample = `# Downstream

A **native** Markdown view with *emphasis*, ~~strikethrough~~, \`inline code\`, and a [link](https://example.com/docs "Docs").
Soft line breaks stay visible,
like this one.

## Lists

- First bullet
- Second bullet with **bold**
  - Nested bullet
    - Third level
- [ ] Open task
- [x] Done task

1. One
2. Two
10) Ten

> A quote with a [reference link][ref].
>
> > And a nested quote.

> [!WARNING]
> Admonitions render as quotes with a colored bar.

---

\`\`\`ts
const answer = 42;
console.log(answer);
\`\`\`

| Name | Value | Notes |
| :--- | ----: | :---: |
| alpha | 1 | Short |
| beta | 22 | A much longer cell that wraps inside its clamped column and pushes the table past the screen width |
| [link](https://example.com/cell) | **333** | \`code\` |

Raw <b>HTML</b> stays literal, $x^2$ too, and ![image alt](https://example.com/a.png) shows alt text.
Unsafe [javascript](javascript:alert(1)) links are plain text. Autolinks: https://example.com and <mailto:hi@example.com>.

A footnote reference[^1].

[ref]: https://example.com/ref
[^1]: The footnote text.
`;
