/**
 * Broken-markdown inputs ported from the remend test suite.
 *
 * remend is Vercel's streaming markdown repair pass (Apache-2.0), the one part
 * of Streamdown that runs ahead of the parser. Its own tests are written
 * against the repaired string, so they could not be reused directly. The
 * inputs are what matter: several hundred shapes of markdown that a model can
 * emit mid-token, and that a renderer must survive without inventing syntax.
 *
 * Every input here is a *stream boundary* case. Nothing in this file is a
 * preference about how incomplete markdown should look, so the assertions in
 * `streaming-preview.test.ts` are invariants rather than expected output. The
 * inputs that do carry a documented opinion are listed separately in that file.
 *
 * Provenance: github.com/vercel/streamdown, packages/remend/__tests__. The
 * inputs are markdown syntax, which is not copyrightable, and are reproduced
 * here under Apache-2.0 with this notice.
 */

/** broken variants */
export const brokenVariants: readonly string[] = [
  '**bold then *italic then ~~strike',
  '~~strike **bold *italic',
  '*italic **bold ~~strike `code',
  '**bold ~~strike',
  '*italic **bold',
  'text*',
  'text~',
  'text$',
  '**bold*',
  '~~strike~',
  '$$formula$',
  '\\*not italic',
  '\\\\*actually italic',
  '\\**not bold',
  '\\*escaped\\* but *real italic',
  '[link1 and [link2',
  '[first](url1) and [second',
  '[outer [inner]',
  '[incomplete link',
  '[**bold link**](incomplete-url',
  '[*italic link*](incomplete',
  '[`code link`](incomplete',
  '[**bold link',
  '> > **deeply nested bold',
  '> * list with **bold',
  '> > > triple nested *italic',
  '> ~~struck text',
  '- [ ] **bold task',
  '- [x] completed ~~struck~~',
  '- [ ] *italic task',
  '- [ ] `code task',
  '| **bold | next |',
  '| `code | next |',
  'text <!-- incomplete comment',
  "text <script>alert('",
  'text <div class="test',
  'text <br>',
  'text <!-- comment -->',
  '$$\\frac{x}{y',
  '$$\\begin{matrix} a',
  '$x^2 + y^2',
  'The price is $50 and $100',
  '$$\n\\sum_{i=0}^{n} x_i',
  '**bold** then **more',
  '`code` then `more',
  '~~done~~ and ~~undone',
  '*first* and *second',
  'paragraph1\n\n**bold',
  'line1\n\n*italic text',
  'text\n\n\n**bold',
  'text\n\n`code',
  '**bold *italic ~~strike `code',
  '***bold-italic ~~strike `code',
  '**bold and *italic',
  '**中文粗体',
  '*日本語',
  '`한국어 코드',
  '~~🎉 celebration',
  '**Hello 世界',
  '---\n**bold after rule',
  '# Heading\n**bold',
  '> quote\n**bold',
  '```\ncode\n```\n*italic',
  '[text][',
  '    *asterisks in indented',
  '    **bold in indented',
  '```\ncode\n```\n**bold',
  '```\nblock\n```\n`inline',
  '****text',
  '*****text',
  '*a**b',
  '**bold\twith\ttabs',
  '**bold\r\nwith CRLF',
  '\n\n\n**bold',
  'text ',
  'text  ',
  '**bold ',
  '**bold text',
  '**bold *italic',
  '**bold *italic `code ~~strike',
  '[link text',
  '$$formula',
  '1. First\n2. **Second item with bold',
  'The function `getData` returns a **Promise',
  'Check the [documentation',
  '```js\nconst x = 1;\n```\n\nThis creates a **variable',
  '- Use `map` to transform\n- Use `filter',
  '## Important *note',
  "Here's the diagram:\n\n![architecture",
  'See ![diagram](http://example.com/img',
  '[click here](https://example.com) for **more',
];

/** comparison operators */
export const comparisonOperators: readonly string[] = [
  '- > 25: rich',
  '* > 25: rich',
  '+ > 25: rich',
  '1. > 25: rich',
  '2) > 10: high',
  '  - > 25: rich',
  '    - > 5: expensive',
  '- >= 10: high',
  '- > $100: expensive',
  '> Some blockquote',
  '> 25 is a number',
  '- > Some quoted text',
  '- > Read more about this',
  '>25',
  '- >25: rich',
];

/** coverage gaps */
export const coverageGaps: readonly string[] = [
  ' ',
  '__content_',
  '_text**',
  '```\n***bold',
  '```\n***\n```\n***text',
  '```\n_code\n```\n_text',
  '\n=',
  '\n==',
  'a~~b~~text',
  'a~~b~~c~',
  '```\n__code\n```\n__text',
  '](partial',
  '![img [text',
  '[link](url) [incomplete',
  '[text] [incomplete',
  '[link](url) _word',
  'func(_arg',
  'div> _text',
  '3<5 _text',
  '<div>\n_text',
  '[link](a_b) _word',
  '```\n__content_',
  '__a__ __b__content_',
  '[a]( b](c [incomplete',
];

/** custom handler shapes */
export const customHandlerShapes: readonly string[] = [
  'foo',
  '**bold',
  'test',
  'test\n-',
  'aaa',
  'hello',
  '<<<JOKE>>>Why did the chicken',
  '<<<JOKE>>>complete<<</JOKE>>>',
];

/** fenced code */
export const fencedCode: readonly string[] = [
  '```javascript\nconst x = 5;',
  '```\ncode here',
  '```python\ndef hello():',
  'Some text\n```js\nconsole.log',
];

/** intraword emphasis */
export const intrawordEmphasis: readonly string[] = [
  'foo*bar',
  'hello*world',
  'test*123*test',
  '*foo*bar*baz',
  '*file*name*ext',
  'before *a*b after *c',
  '*기울임*으로 *another',
  '*foo* bar*baz',
];

/** list markers */
export const listMarkers: readonly string[] = [
  '- Item 1\n- Item 2 with **bol',
  '- __',
  '- **',
  '- __\n- **',
  '\n- __\n- **',
  '* __\n* **',
  '+ __\n+ **',
  '- __ text after',
  '- ** text after',
  '- __\n- Normal item\n- **',
  '- ***',
  '- *',
  '- _',
  '- ~~',
  '- `',
  '- **text\nmore text',
  '* **content\n* Another item',
];

/** marker edge cases */
export const markerEdgeCases: readonly string[] = [
  'Text ending with *',
  'Text ending with **',
  '****',
  '``',
  '**',
  '__',
  '***',
  '*',
  '_',
  '~~',
  '`',
  '** __',
  '\n** __\n',
  '* _ ~~ `',
  '** ',
  ' **',
  '  **  ',
  '**text',
  '__text',
  '*text',
  '_text',
  '~~text',
  '`text',
  'text**',
  'text`',
  'text~~',
  'text **bold',
  'text\n**bold',
  'text\t`code',
  '**émoji 🎉',
  '`código',
  '**&lt;tag&gt;',
  '`&amp;',
  '3 + 2 - 5 * 0 = ?',
  '5 * 0',
  'x * y',
  'a * b = c',
  '2 * 3 * 4',
  '5 * 0 and *italic',
];

/** mixed markers */
export const mixedMarkers: readonly string[] = [
  'Text with [link and **bold',
  '*italic with **bold',
  '**bold with `code',
  '~~strike with **bold',
  '**bold with $x^2',
  '**bold and *bold-italic***',
  'combined **_bold and italic',
  '**_text',
  '_italic and **bold',
];

/** setext underlines */
export const setextUnderlines: readonly string[] = [
  'here is a list',
  'here is a list\n',
  'here is a list\n-',
  'here is a list\n- list item 1',
  'This is a title\n=',
  'This is a title\n==',
  'This is a title\n===',
];

/** single tilde between words */
export const singleTildeBetweenWords: readonly string[] = [
  '20~25°C',
  '20~25°C。20~25°C',
  'foo~bar',
  '~~strikethrough~~',
  '~hello',
  'hello~',
  'hello ~ world',
  '```\n20~25\n```',
  '`20~25`',
  '20~25 and ~~strike',
  '日本~語',
  'α~β',
  'é~x',
  '𐐀~a',
  'foo~bar~baz',
];

/** streamed chunk boundaries */
export const streamedChunkBoundaries: readonly string[] = [
  'This is **bold with *ital',
  '**bold _und',
  '# Main Title\n## Subtitle with **emph',
  '> Quote with **bold',
  '| Col1 | Col2 |\n|------|------|\n| **dat',
  '1. First item\n   - Nested with `code\n2. Second',
  'Text **bold `code',
];

/** thematic breaks */
export const thematicBreaks: readonly string[] = [
  '---',
  '----',
  '-----',
  '*****',
  '___',
  '____',
  '_____',
  '- - -',
  '* * *',
  '_ _ _',
  '-  -  -',
  '*   *   *',
  '_    _    _',
  'Text before\n***\nText after',
  'Text before\n___\nText after',
  'Some text\n\n---',
  'Some text\n\n***',
  'Some text\n\n___',
  '---\n\nSome text',
  '***\n\nSome text',
  '___\n\nSome text',
  'Section 1\n\n---\n\nSection 2\n\n---\n\nSection 3',
  'Text with --',
  '--',
  'Text\n\n--',
  '   ---',
  '  ***',
  ' ___',
  'This is not a --- horizontal rule',
  'Text\n***',
];

/** unterminated bold */
export const unterminatedBold: readonly string[] = [
  'Text with **bold',
  '**incomplete',
  '**first** and **second',
  'Here is some **bold tex',
  '**xxx*',
  '**bold text*',
  'Text with **bold*',
  'This is **bold text*',
];

/** unterminated bold italic */
export const unterminatedBoldItalic: readonly string[] = [
  'Text with ***bold-italic',
  '***incomplete',
  '***first*** and ***second',
  '*italic* **bold** ***both',
  '***Starting bold-italic',
  '***bold-italic with `code',
  'text ***',
  'text ****',
  'text *****',
  'text ******',
  'text***',
  'word****',
  'end******',
  '***start***end***',
  '***text***',
  '***word text***',
  'Combined **bold and *italic*** text',
  '**bold and *italic*** more text',
  'test **bold and *italic*** end',
  '- Combined **bold and *italic*** text',
];

/** unterminated html */
export const unterminatedHtml: readonly string[] = [
  'Hello <div',
  'Hello <custom',
  'Hello <casecard',
  'Text <MyComponent',
  'Hello </div',
  'Hello </custom',
  '<div>content</di',
  'Hello <div class="foo',
  'Hello <div class=',
  'Hello <a href="https://example.com',
  '<custom data-id',
  'Hello <div>',
  '<div>content</div>',
  '<br/>',
  "<img src='test'>",
  '3 < 5',
  'x < y',
  'if a <',
  'value <1',
  '```\n<div\n```',
  '```html\n<custom',
  '`<div`',
  '<div',
  '<custom',
  '</div',
  'Some text here\n\n<casecard',
  '# Heading\n\nParagraph <custom',
  '<div>Hello</div> <span',
  '<a target="_blank" href="https://link.com">word</a>',
  '<a target="_blank">link</a>',
  '<iframe src="x" sandbox="allow_scripts">',
  '$x<y$ then <div',
  '$$a<b$$ done <span',
];

/** unterminated images */
export const unterminatedImages: readonly string[] = [
  'Text with ![incomplete image',
  '![partial',
  'See ![the diag',
  '![logo](./assets/log',
  'Text ![outer [inner]',
  '![nested [brackets] text',
  'Start ![foo [bar] baz',
];

/** unterminated inline code */
export const unterminatedInlineCode: readonly string[] = [
  'Text with `code',
  '`incomplete',
  '\\`not code\\` **bold',
  '\\` *italic',
  '`**bold`',
  '`*italic`',
  '`~~strikethrough`',
  '*italic',
  '~~strike',
  '`code` **bold',
];

/** unterminated italic */
export const unterminatedItalic: readonly string[] = [
  'Text with __italic',
  '__incomplete',
  '__first__ and __second',
  '__xxx_',
  '__bold text_',
  'Text with __bold_',
  'This is __bold text_',
  'Text with *italic',
  '*incomplete',
  '**bold** and *italic',
  '234234*123',
  '*italic with some*var*name inside',
  'test*var and *incomplete italic',
  '\\*escaped asterisk and *italic',
  '*start \\* middle \\* end',
  'abc*123',
  '123*abc',
  'This is *italic',
  '*word* and more text',
  'Text with _italic',
  '_incomplete',
  '__bold__ and _italic',
  '\\_escaped\\_ and _unescaped',
  'Start \\_escaped\\_ middle _incomplete',
  '\\_fully\\_escaped\\_',
  '\\_escaped\\_ _complete_ pair',
  'café_price',
  'naïve_approach',
  'some_variable_name',
  'test_123_value',
  '_start with underscore',
  '_italic with some_var_name inside',
  'test_var and _incomplete italic',
  'Text with _italic\n',
  '_incomplete\n\n',
  'Start _text\n',
];

/** unterminated links */
export const unterminatedLinks: readonly string[] = [
  'Text with [incomplete link',
  'Text [partial',
  '[outer [nested] text](incomplete',
  '[link with [inner] content](http://incomplete',
  'Text [foo [bar] baz](',
  'Check out [this lin',
  'Visit [our site](https://exa',
  'Text [outer [inner',
  '[foo [bar [baz',
  'Text [outer [inner]',
  '[link [nested] text',
  'Text ![incomplete image',
  'Text ![alt](http://partial',
];

/** unterminated math */
export const unterminatedMath: readonly string[] = [
  'Text with $$formula',
  '$$incomplete',
  '$$first$$ and $$second',
  '$$x + y = z',
  '$$x = y$',
  '$$\nx = 1\ny = 2',
  'Text with $formula',
  '$incomplete',
  '$first$ and $second',
  '$$block$$ and $inline',
  '$x + y = z',
  '$$$',
  '$$$$',
  '$$block$$ then $x + y',
];

/** unterminated strikethrough */
export const unterminatedStrikethrough: readonly string[] = [
  'Text with ~~strike',
  '~~incomplete',
  '~~first~~ and ~~second',
  '~~xxx~',
  '~~strike text~',
  'Text with ~~strike~',
  'This is ~~strikethrough~',
];

/** Every ported input, in concern order. */
export const CORPUS: readonly string[] = [
  ...brokenVariants,
  ...comparisonOperators,
  ...coverageGaps,
  ...customHandlerShapes,
  ...fencedCode,
  ...intrawordEmphasis,
  ...listMarkers,
  ...markerEdgeCases,
  ...mixedMarkers,
  ...setextUnderlines,
  ...singleTildeBetweenWords,
  ...streamedChunkBoundaries,
  ...thematicBreaks,
  ...unterminatedBold,
  ...unterminatedBoldItalic,
  ...unterminatedHtml,
  ...unterminatedImages,
  ...unterminatedInlineCode,
  ...unterminatedItalic,
  ...unterminatedLinks,
  ...unterminatedMath,
  ...unterminatedStrikethrough,
];

/**
 * Inputs larger than this are left to the oracle's own long fixtures. The
 * ported corpus is deliberately made of small shapes, so a slow case here
 * means a parser regression rather than a large document.
 */
export const MAX_INPUT_BYTES = 512;
