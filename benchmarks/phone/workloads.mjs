// Keep ten prompt slots so existing captures can still be replayed. New captures
// replace the travel reply with feature permutations, without extra requests.
export const WORKLOADS = {
  'ai-single': {
    responses: [0],
    label: 'One AI reply (prose, table, TypeScript)',
  },
  'ai-code-heavy': {
    responses: [4],
    label: 'One code-heavy AI reply (14 code blocks)',
  },
  'ai-feature-matrix': {
    responses: [3],
    label: 'Math, code and tables: every subset and ordering',
    math: true,
  },
  'ai-concurrent-5': {
    responses: [0, 1, 2, 3, 4],
    label: 'Five AI replies streaming at once, including the feature matrix',
    math: true,
  },
  // Huge replies stream for minutes, so they run once per library, without warmup.
  'ai-huge-single': {
    responses: [5],
    label: 'One huge AI reply (60 points, mixed features)',
    math: true,
    repeats: 1,
  },
  'ai-huge-concurrent-5': {
    responses: [5, 6, 7, 8, 9],
    label: 'Five huge AI replies (60 points each, mixed features)',
    math: true,
    repeats: 1,
  },
};

// Exhaustive for these three block families: 1 empty + 3 singles + 6 ordered
// pairs + 6 triples. Packing them into one reply avoids 16 separate phone runs.
function featureOrders(remaining, prefix = []) {
  return [
    prefix,
    ...remaining.flatMap((feature) =>
      featureOrders(
        remaining.filter((other) => other !== feature),
        [...prefix, feature]
      )
    ),
  ];
}
export const FEATURE_ORDERS = featureOrders(['math', 'code', 'table']);

const RULES =
  'Answer in GitHub-flavored Markdown. Do not use images or raw HTML. Use dollar-delimited LaTeX only when mathematics is requested. Keep Markdown examples outside code fences so they actually render.';
const MATRIX = `Write a compact Markdown rendering test about calculating travel costs, about 700 words. Produce exactly these 16 sections, in this order, using each entry verbatim as a level-two heading:\n${FEATURE_ORDERS.map((order) => order.join(' then ') || 'prose only').join('\n')}\nFor each section, emit only the named feature blocks, exactly once each and in the heading's order, separated by blank lines. Math means a short paragraph containing inline $x^2$ and a separate $$ display equation. Code means a fenced block of two to four lines; rotate javascript, typescript, python and json. Table means a GFM table with aligned columns and two data rows. The prose-only section contains plain prose and none of those features. Do not wrap the whole reply in a code fence or add explanations between feature blocks. Use Unicode café, 清水寺 and 😀 in the prose-only section. Put bold, italic, strikethrough, inline code, and links in table cells to test nested formatting. Put a dollar sign, backticks and a pipe inside a code string to check that code remains literal. ${RULES}`;

const HUGE_TOPICS = [
  'the engineering and history of bridges',
  'how databases store, index and query data',
  'the science of the oceans',
  'the history of space exploration',
  'how computers evolved from the abacus to smartphones',
];
export const PROMPTS = [
  `Explain how to build a retry-safe background job in about 600 words. Use headings, a numbered list, a bullet list, a GFM table comparing three retry policies, bold and italic emphasis, an inline code span, and a TypeScript code block of about 15 lines. Put inline code and a link in table cells, and mix bold, italic and links inside list items. ${RULES}`,
  `Compare PostgreSQL and MongoDB for a chat application's message storage in about 600 words. Include a comparison table with five rows, a Python code block that inserts and pages through messages, a short blockquote containing a nested list and inline code, and a recommendation section with a bullet list. Include reference-style links, a bare URL and a footnote. ${RULES}`,
  `Write a step-by-step guide to finding and fixing dropped frames in a React Native app, about 600 words. Use nested numbered and bullet lists, a task list checklist with nested items, a JSON configuration code block, a bash code block, a GFM warning alert, and links to https://reactnative.dev/docs/performance. Include a horizontal rule, escaped emphasis delimiters and an HTML entity. ${RULES}`,
  MATRIX,
  `Teach an interactive git rebase workflow for cleaning up a feature branch in about 600 words. Include exactly 14 short fenced code blocks, mostly bash plus examples in javascript, typescript, python, json, diff and an unlabeled fence. Alternate code blocks with a numbered list of steps, a table of common rebase commands, inline code for flags, a warning blockquote, and a reference-style link to https://git-scm.com/docs/git-rebase. ${RULES}`,
  // Indices 5-9 stay huge replies, one per topic. Rotate block order per section.
  ...HUGE_TOPICS.map(
    (topic, index) =>
      `Write exactly 60 numbered points about ${topic}. Group them under six "##" headings of ten points each. Start every point with a short bold title, then two or three sentences. In each section also add one short code block, one GFM table and one display math equation relevant to the topic. Rotate their order across sections using these six permutations: ${FEATURE_ORDERS.filter(
        (order) => order.length === 3
      )
        .map((_, section, triples) =>
          triples[(section + index) % triples.length].join(' then ')
        )
        .join(
          '; '
        )}. Include inline math, nested lists, task items, blockquotes, links and Unicode within the numbered points. After the 60 points, add a Markdown table summarizing the six sections and a short closing paragraph. ${RULES}`
  ),
];
