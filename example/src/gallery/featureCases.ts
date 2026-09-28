/** Focused cases for checking native layout on a device. */
export const featureCases = [
  {
    id: 'rtl-and-tasks',
    title: 'RTL, nested lists, and task state',
    markdown: `## مهام اليوم

> فقرة عربية مع [رابط](https://example.com/rtl) وكلمة English.

- [ ] مهمة مفتوحة
- [x] مهمة مكتملة
  - عنصر متداخل مع **نص عريض**

1. पहला चरण
2. दूसरा चरण`,
  },
  {
    id: 'wide-table',
    title: 'Wide table and linked cells',
    markdown: `| Column | Details | Link | Status | Notes |
| :--- | :--- | :---: | ---: | :--- |
| Alpha | A cell that wraps when the column reaches its width limit | [Open](https://example.com/alpha) | 1 | First row |
| Beta | Another long cell that should keep the same column width | [Open](https://example.com/beta) | 2 | Second row |
| Gamma | A third row for checking horizontal scroll after an update | [Open](https://example.com/gamma) | 3 | Third row |`,
  },
  {
    id: 'code-and-fallbacks',
    title: 'Long code and safe fallbacks',
    markdown: `## Plain code

\`\`\`ts
${Array.from(
  { length: 48 },
  (_, index) => `const value${index} = ${index};`
).join('\n')}
\`\`\`

![Read the alt text](https://example.com/image.png)

Raw <button>HTML</button> and $x^2$ stay visible as text.

[Unsafe link](javascript:alert(1)) stays plain text.`,
  },
] as const;
