/* eslint-disable no-bitwise -- inline styles are a bitmask */
/**
 * Renders Markdown the way the native view does, as React elements.
 *
 * `DownstreamMarkdown` hands a source and a session sequence to a native view.
 * Asserting on the props it passes proves the wiring; it does not prove a reader
 * sees the right text. This stands in for the native view by running the real
 * engine over that source and returning the blocks as elements, with the same
 * element per block kind the iOS and Android renderers use.
 *
 * The parse is real. The layout is text, not TextKit or StaticLayout.
 */
import { createElement, type ReactElement, type ReactNode } from 'react';

import { parse, KIND, STYLE, type Block, type RenderModel } from './engine';
import { sourceForSession } from './real-engine-module';

const TAG: Record<number, string> = {
  [KIND.paragraph]: 'p',
  [KIND.code]: 'pre',
  [KIND.html]: 'p',
  [KIND.hr]: 'hr',
  [KIND.table]: 'table',
  [KIND.heading]: 'h1',
};

function headingTag(block: Block): string {
  const level = Math.min(6, Math.max(1, block.headingLevel || 1));
  return `h${level}`;
}

/**
 * Wraps a block's text in the elements its inline styles call for. Runs arrive
 * as `[start, end, styleBits, linkIndex]` tuples.
 */
function styledText(block: Block): ReactNode[] {
  const runs = [...block.runs].sort((a, b) => a[0] - b[0]);
  const nodes: ReactNode[] = [];
  let cursor = 0;
  for (const [start, end, styles] of runs) {
    if (start > cursor) nodes.push(block.text.slice(cursor, start));
    const text = block.text.slice(start, end);
    let wrapped = false;
    // Strongest first, so nested styles nest the way a reader expects.
    for (const [bit, tag] of [
      [STYLE.code, 'code'],
      [STYLE.strikethrough, 's'],
      [STYLE.strong, 'strong'],
      [STYLE.emphasis, 'em'],
    ] as const) {
      if ((styles & bit) === 0) continue;
      nodes.push(createElement(tag, { key: `${start}-${end}` }, text));
      wrapped = true;
      break;
    }
    if (!wrapped) nodes.push(text);
    cursor = end;
  }
  if (cursor < block.text.length) nodes.push(block.text.slice(cursor));
  return nodes;
}

function renderTable(block: Block, index = 0): ReactElement {
  // A cell count alone cannot say where the rows break, so the engine's own
  // table grouping is used: one row per block, cells in order.
  const cells = block.cells.map(([start, end]) =>
    createElement('td', { key: start }, block.text.slice(start, end))
  );
  return createElement(
    'table',
    { key: index },
    createElement('tr', null, ...cells)
  );
}

function renderBlock(block: Block, index: number): ReactElement {
  if (block.kind === KIND.table) {
    return renderTable(block);
  }
  if (block.kind === KIND.hr) return createElement('hr', { key: index });
  const tag =
    block.kind === KIND.heading ? headingTag(block) : (TAG[block.kind] ?? 'p');
  const props: Record<string, unknown> = { key: index };
  if (block.kind === KIND.code && block.language) {
    props['data-language'] = block.language;
  }
  return createElement(
    tag,
    props,
    block.kind === KIND.code ? block.text : styledText(block)
  );
}

function renderModel(model: RenderModel): ReactElement[] {
  return model.blocks.map(renderBlock);
}

export interface NativeView {
  /** The stand-in for the native component. */
  component(props: Record<string, unknown>): ReactElement;
  /** Every commit the native view received, oldest first. */
  records: Record<string, unknown>[];
  count(): number;
  last(): Record<string, unknown> | undefined;
  reset(): void;
}

/**
 * Builds the stand-in. The rendered tree is returned to React, so a test reads
 * it by querying the mounted container rather than through this handle.
 */
export function createNativeView(): NativeView {
  const records: Record<string, unknown>[] = [];
  return {
    component(props: Record<string, unknown>): ReactElement {
      records.push(props);
      // A stream view is handed a session, not a source, exactly as the native
      // view is. The session's real source comes from the engine, so what is
      // rendered is what the worker produced.
      const sessionId =
        typeof props.sessionId === 'string' ? props.sessionId : '';
      const markdown =
        sessionId !== ''
          ? sourceForSession(sessionId)
          : typeof props.markdown === 'string'
            ? props.markdown
            : '';
      // The slot children are rendered after the document, exactly as Fabric
      // mounts them: they are the view's children, positioned over the blocks
      // they replace, so a test can see what a renderer mounted and that a
      // fenced block still carries its text.
      // The slot children live in their own element, which is what Fabric does:
      // a slot is a child of the view, not of whichever block happens to precede
      // it. Without this wrapper a growing document would shift the slot array's
      // position among its siblings and every renderer would remount with it.
      const children = props.children as ReactNode | undefined;
      return createElement(
        'div',
        { 'data-native-view': 'true' },
        ...renderModel(parse(markdown)),
        ...parse(markdown).links.map((link, index) =>
          createElement('a', { key: `link-${index}`, href: link.url })
        ),
        children === undefined
          ? null
          : // Keyed, so it keeps its place among its siblings however many blocks
            // the document grows by: Fabric mounts a slot by its own index in the
            // host layer, not by its position after the document's blocks.
            createElement(
              'div',
              { 'data-slots': 'true', 'key': 'slots' },
              children
            )
      );
    },
    records,
    count: () => records.length,
    last: () => records.at(-1),
    reset() {
      records.length = 0;
    },
  };
}
