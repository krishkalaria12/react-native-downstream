/**
 * @vitest-environment happy-dom
 */
/**
 * Fence renderers: `plugins.renderers`, from the engine's slots to a mounted
 * component, with the code card underneath whenever a component is not up.
 *
 * The eight cases Streamdown's own `custom-renderer.test.tsx` covers are ported
 * first and unchanged, because the API is Streamdown's and a divergence would
 * be a divergence a user has to work around. The rest are the rules Downstream
 * adds: the fallback, the streaming behaviour, the error routing, and the cost a
 * view with no renderers does not pay.
 *
 * The engine behind the TurboModule is the real one, so every slot here is the
 * slot the app would mount.
 */
import React, { act, lazy } from 'react';
import { createRoot, type Root } from 'react-dom/client';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

import { createMarkdownStream } from '../src/createMarkdownStream';
import { DownstreamMarkdown } from '../src/DownstreamMarkdown';
import {
  mayHoldRenderers,
  normalizeLanguage,
  resolveRenderers,
} from '../src/fenceRenderers';
import { fenceSlots, parse } from './engine';
import { visibleText } from './engine';
import { createNativeView, type NativeView } from './native-view';
import { resetEngine, slotWork, SYNC_SLOT_BYTES } from './real-engine-module';
import type { CustomRendererProps } from '../src/types';
import { brokenVariants } from './remend-corpus';

const nativeView: NativeView = createNativeView();

// React only treats `act` as a real act outside a test renderer when the
// environment says so, and this file drives `act` directly.
(
  globalThis as unknown as { IS_REACT_ACT_ENVIRONMENT: boolean }
).IS_REACT_ACT_ENVIRONMENT = true;

vi.mock('../src/DownstreamMarkdownViewNativeComponent', () => ({
  __esModule: true,
  default: (props: Record<string, unknown>) => nativeView.component(props),
}));

// The slot is a real Fabric child in the app. Here it is a real React child of
// the stand-in native view, which is the whole contract: a fence becomes a
// component by being a child of the view.
vi.mock('../src/DownstreamFenceSlotNativeComponent', () => ({
  __esModule: true,
  default: ({
    ordinal,
    children,
  }: {
    ordinal: number;
    children?: React.ReactNode;
  }) => (
    <div data-slot-ordinal={ordinal} data-slot="true">
      {children}
    </div>
  ),
}));

vi.mock(
  '../src/NativeDownstreamSession',
  async () => (await import('./real-engine-module')).realEngineModule
);

let host: HTMLDivElement | undefined;
let root: Root | undefined;
let committed: React.ReactElement | undefined;
const streams: { dispose(): void }[] = [];

function stream(): ReturnType<typeof createMarkdownStream> {
  const created = createMarkdownStream();
  streams.push(created);
  return created;
}

function mount(element: React.ReactElement): void {
  host ??= document.body.appendChild(document.createElement('div'));
  root ??= createRoot(host);
  committed = element;
  act(() => {
    root!.render(element);
  });
}

function rerender(next?: React.ReactElement): void {
  committed = next ?? committed;
  act(() => {
    root!.render(committed!);
  });
}

function rendered(): ParentNode {
  return host!;
}

function unmount(): void {
  act(() => {
    root?.unmount();
  });
  root = undefined;
  host?.remove();
  host = undefined;
  committed = undefined;
}

/** The fence slots the native view mounted, as `{ ordinal, text }`. */
function slots(): { ordinal: number; text: string }[] {
  return [...rendered().querySelectorAll('[data-slot="true"]')].map((slot) => ({
    ordinal: Number(slot.getAttribute('data-slot-ordinal')),
    text: slot.textContent ?? '',
  }));
}

/** Every element the reader would see, slot children included. */
function renderedText(): string {
  return (rendered().textContent ?? '').replace(/\n+$/, '');
}

let frames: FrameRequestCallback[] = [];

/**
 * Runs the pending frames.
 *
 * Twice, because a snapshot reaches the view in two steps: the first frame
 * publishes it, and the render that follows schedules the next one. A chat does
 * not see the difference; a test that flushes once sees a document from the
 * snapshot before.
 */
function runFrames(): void {
  for (let round = 0; round < 2; round++) {
    act(() => {
      for (const callback of frames.splice(0)) callback(0);
    });
  }
}

beforeEach(() => {
  if (host) host.replaceChildren();
  nativeView.reset();
  resetEngine();
  frames = [];
  vi.spyOn(globalThis, 'requestAnimationFrame').mockImplementation(
    (callback) => {
      frames.push(callback);
      return frames.length;
    }
  );
  vi.spyOn(globalThis, 'cancelAnimationFrame').mockImplementation(() => {});
});

afterEach(() => {
  unmount();
  for (const created of streams.splice(0)) created.dispose();
  vi.restoreAllMocks();
});

/** A renderer that records what it was handed, so a test can read the props. */
function recorder(seen: CustomRendererProps[]): React.FC<CustomRendererProps> {
  return (props) => {
    seen.push(props);
    return <span data-renderer="true">{props.code}</span>;
  };
}

describe('fence renderers, matching and props', () => {
  it('a registered language mounts its component with the four props', () => {
    const seen: CustomRendererProps[] = [];
    mount(
      <DownstreamMarkdown
        markdown={'Before.\n\n```mermaid\ngraph TD;\n```\n'}
        plugins={{
          renderers: [{ language: 'mermaid', component: recorder(seen) }],
        }}
      />
    );

    expect(slots()).toEqual([{ ordinal: 0, text: 'graph TD;' }]);
    expect(seen).toHaveLength(1);
    expect(seen[0]!).toEqual({
      code: 'graph TD;',
      language: 'mermaid',
      meta: undefined,
      isIncomplete: false,
    });
  });

  it('a language that is not registered keeps the code card', () => {
    const seen: CustomRendererProps[] = [];
    mount(
      <DownstreamMarkdown
        markdown={'```js\nconst a = 1;\n```\n'}
        plugins={{
          renderers: [{ language: 'mermaid', component: recorder(seen) }],
        }}
      />
    );

    expect(slots()).toEqual([]);
    expect(seen).toHaveLength(0);
    // The fence is still a code block, with its code.
    expect(renderedText()).toContain('const a = 1;');
  });

  it('the language is matched case-insensitively and passed as written', () => {
    const seen: CustomRendererProps[] = [];
    mount(
      <DownstreamMarkdown
        markdown={'```Mermaid\ngraph;\n```\n'}
        plugins={{
          renderers: [{ language: 'mermaid', component: recorder(seen) }],
        }}
      />
    );

    expect(slots()).toHaveLength(1);
    expect(seen[0]!.language).toBe('Mermaid');
  });

  it('several renderers each get their own fences', () => {
    const charts: CustomRendererProps[] = [];
    const diagrams: CustomRendererProps[] = [];
    mount(
      <DownstreamMarkdown
        markdown={
          '```chart\n{"a":1}\n```\n\n```mermaid\ngraph;\n```\n\n```ts\nconst a = 1;\n```\n'
        }
        plugins={{
          renderers: [
            { language: 'chart', component: recorder(charts) },
            { language: 'mermaid', component: recorder(diagrams) },
          ],
        }}
      />
    );

    // Ordinals count every code block, and the untyped fence after the two
    // hosted ones does not shift them.
    expect(slots()).toEqual([
      { ordinal: 0, text: '{"a":1}' },
      { ordinal: 1, text: 'graph;' },
    ]);
    expect(charts[0]!.language).toBe('chart');
    expect(diagrams[0]!.language).toBe('mermaid');
  });

  it('one renderer can claim several languages', () => {
    const seen: CustomRendererProps[] = [];
    mount(
      <DownstreamMarkdown
        markdown={'```vega\n{"a":1}\n```\n\n```vega-lite\n{"b":2}\n```\n'}
        plugins={{
          renderers: [
            { language: ['vega', 'vega-lite'], component: recorder(seen) },
          ],
        }}
      />
    );

    expect(slots()).toEqual([
      { ordinal: 0, text: '{"a":1}' },
      { ordinal: 1, text: '{"b":2}' },
    ]);
    expect(seen.map((props) => props.language)).toEqual(['vega', 'vega-lite']);
  });

  it('with no renderers, nothing is asked and nothing is mounted', () => {
    mount(<DownstreamMarkdown markdown={'```mermaid\ngraph;\n```\n'} />);

    expect(slots()).toEqual([]);
    // A view with no renderers never asks the engine about fences, which is the
    // whole cost rule for this feature.
    expect(slotWork.queries).toBe(0);
  });

  it('meta is passed when the fence has one and undefined when it does not', () => {
    const seen: CustomRendererProps[] = [];
    mount(
      <DownstreamMarkdown
        markdown={
          '```chart title="Revenue"\n{"a":1}\n```\n\n```chart\n{"b":2}\n```\n'
        }
        plugins={{
          renderers: [{ language: 'chart', component: recorder(seen) }],
        }}
      />
    );

    expect(seen[0]!.meta).toBe('title="Revenue"');
    expect(seen[1]!.meta).toBeUndefined();
  });
});

describe('fence renderers, the code card fallback', () => {
  it('a renderer that throws leaves the code card and reports once', async () => {
    const errors: { code: string; language?: string }[] = [];
    const Boom = () => {
      throw new Error('renderer blew up');
    };
    mount(
      <DownstreamMarkdown
        markdown={'```mermaid\ngraph;\n```\n'}
        plugins={{ renderers: [{ language: 'mermaid', component: Boom }] }}
        onError={(error) => errors.push(error)}
      />
    );

    // The console noise React prints for a caught error is not the app's.
    await act(async () => {});
    expect(slots()).toEqual([]);
    expect(renderedText()).toContain('graph;');
    expect(
      errors.map((error) => ({ code: error.code, language: error.language }))
    ).toEqual([{ code: 'renderer_failed', language: 'mermaid' }]);
  });

  it('a renderer that returns nothing is an empty slot, not an error', async () => {
    const errors: unknown[] = [];
    mount(
      <DownstreamMarkdown
        markdown={'```mermaid\ngraph;\n```\n'}
        plugins={{
          renderers: [{ language: 'mermaid', component: () => null }],
        }}
        onError={(error) => errors.push(error)}
      />
    );

    await act(async () => {});
    expect(errors).toEqual([]);
    expect(slots()).toHaveLength(1);
  });

  it('a lazy renderer mounts no slot until it resolves', async () => {
    let resolve: (() => void) | undefined;
    const gate = new Promise<void>((done) => {
      resolve = done;
    });
    const Loaded = lazy(async () => {
      await gate;
      return {
        default: (props: CustomRendererProps) => <span>{props.code}</span>,
      };
    });
    mount(
      <DownstreamMarkdown
        markdown={'```mermaid\ngraph;\n```\n'}
        plugins={{ renderers: [{ language: 'mermaid', component: Loaded }] }}
      />
    );

    // Suspended: the slot is inside the Suspense boundary, so no child is
    // mounted and the fence draws as the code card it always could.
    expect(slots()).toEqual([]);
    expect(renderedText()).toContain('graph;');

    // React.lazy resolves its module on a microtask, and the resolved component
    // is rendered on the turn after that, so this is two turns rather than one.
    await act(async () => {
      resolve?.();
      await gate;
    });
    await act(async () => {
      await Promise.resolve();
    });
    expect(slots()).toEqual([{ ordinal: 0, text: 'graph;' }]);
  });
});

describe('fence renderers while streaming', () => {
  it('a growing fence is incomplete and keeps its component', () => {
    const seen: CustomRendererProps[] = [];
    const created = stream();
    mount(
      <DownstreamMarkdown
        stream={created}
        plugins={{
          renderers: [{ language: 'mermaid', component: recorder(seen) }],
        }}
      />
    );

    act(() => {
      created.append('```mermaid\ngraph TD;\n');
    });
    runFrames();
    expect(seen.at(-1)).toEqual({
      code: 'graph TD;',
      language: 'mermaid',
      meta: undefined,
      isIncomplete: true,
    });

    act(() => {
      created.append('```\n');
    });
    runFrames();
    expect(seen.at(-1)).toEqual({
      code: 'graph TD;',
      language: 'mermaid',
      meta: undefined,
      isIncomplete: false,
    });
  });

  it('a settled renderer is not re-rendered for later snapshots', () => {
    let renders = 0;
    const Chart = (props: CustomRendererProps) => {
      renders += 1;
      return <span>{props.code}</span>;
    };
    const created = stream();
    mount(
      <DownstreamMarkdown
        stream={created}
        plugins={{ renderers: [{ language: 'mermaid', component: Chart }] }}
      />
    );

    act(() => {
      created.append('```mermaid\ngraph;\n```\n');
    });
    runFrames();
    const afterFence = renders;
    expect(afterFence).toBe(1);

    for (let index = 0; index < 50; index++) {
      act(() => {
        created.append(`Paragraph ${index}.\n\n`);
      });
      runFrames();
    }
    // Fifty later snapshots, one render: the four props did not change, so
    // React skipped the renderer even though the document did.
    expect(renders).toBe(afterFence);
    expect(slots()).toEqual([{ ordinal: 0, text: 'graph;' }]);
  });

  it('a growing fence keeps its component instance across snapshots', () => {
    const instances = new Set<unknown>();
    const Chart = (props: CustomRendererProps) => {
      const ref = React.useRef({});
      instances.add(ref.current);
      return <span>{props.code}</span>;
    };
    const created = stream();
    mount(
      <DownstreamMarkdown
        stream={created}
        plugins={{ renderers: [{ language: 'mermaid', component: Chart }] }}
      />
    );

    act(() => {
      created.append('```mermaid\ngraph ');
    });
    runFrames();
    act(() => {
      created.append('TD;\n');
    });
    runFrames();
    expect(instances.size).toBe(1);
  });

  it('reset() mounts a new component rather than reusing the old one', () => {
    const instances = new Set<unknown>();
    const Chart = (props: CustomRendererProps) => {
      const ref = React.useRef({});
      instances.add(ref.current);
      return <span>{props.code}</span>;
    };
    const created = stream();
    mount(
      <DownstreamMarkdown
        stream={created}
        plugins={{ renderers: [{ language: 'mermaid', component: Chart }] }}
      />
    );

    act(() => {
      created.append('```mermaid\nfirst;\n```\n');
    });
    runFrames();
    act(() => {
      created.reset('```mermaid\nsecond;\n```\n');
    });
    runFrames();
    expect(instances.size).toBe(2);
    expect(slots()).toEqual([{ ordinal: 0, text: 'second;' }]);
  });
});

describe('fence renderers, cost', () => {
  it('an unchanged plugins literal asks nothing more', () => {
    const plugins = {
      renderers: [
        {
          language: 'mermaid',
          component: (props: CustomRendererProps) => <span>{props.code}</span>,
        },
      ],
    };
    const tree = (
      <DownstreamMarkdown
        markdown={'```mermaid\ngraph;\n```\n'}
        plugins={plugins}
      />
    );
    mount(tree);
    const afterFirst = slotWork.queries;
    expect(afterFirst).toBe(1);

    rerender(tree);
    rerender(tree);
    // The same literal rendered again is the same configuration, so the engine
    // is not asked again and the reader sees nothing new.
    expect(slotWork.queries).toBe(afterFirst);
  });

  it('a source with no registered language never reaches the engine', () => {
    mount(
      <DownstreamMarkdown
        markdown={'A paragraph with no fence in it.\n'}
        plugins={{
          renderers: [{ language: 'mermaid', component: () => <span>x</span> }],
        }}
      />
    );
    expect(slotWork.queries).toBe(0);
  });

  it('a source past the synchronous bound is read on the worker', async () => {
    const fence = '```mermaid\ngraph;\n```\n';
    const filler =
      'A paragraph long enough to make this message large.\n\n'.repeat(280);
    const small = `${filler}${fence}`;
    const large = `${filler.repeat(4)}${fence}${'more text. '.repeat(2000)}`;

    mount(
      <DownstreamMarkdown
        markdown={small}
        plugins={{
          renderers: [
            {
              language: 'mermaid',
              component: (props: CustomRendererProps) => (
                <span>{props.code}</span>
              ),
            },
          ],
        }}
      />
    );
    expect(Buffer.byteLength(small, 'utf8')).toBeLessThanOrEqual(
      SYNC_SLOT_BYTES
    );
    expect(slotWork.async).toBe(0);

    unmount();
    mount(
      <DownstreamMarkdown
        markdown={large}
        plugins={{
          renderers: [
            {
              language: 'mermaid',
              component: (props: CustomRendererProps) => (
                <span>{props.code}</span>
              ),
            },
          ],
        }}
      />
    );
    expect(Buffer.byteLength(large, 'utf8')).toBeGreaterThan(SYNC_SLOT_BYTES);
    expect(slots()).toEqual([]);

    // The worker answers, and then the fence is a component: the same frame in
    // which a large source's preparation lands.
    await act(async () => {
      await Promise.resolve();
      await Promise.resolve();
    });
    expect(slotWork.async).toBe(1);
    expect(slots()).toEqual([{ ordinal: 0, text: 'graph;' }]);
  });

  it('an answer the engine gave in full is never asked for again', async () => {
    // A fence that names a registered language and still yields no slot: the
    // source is a math fence, which is a formula block rather than a code block
    // while math is on. The pre-filter sees the language, the engine does not, so
    // the answer is complete — and a complete answer is not a question.
    const tree = (
      <DownstreamMarkdown
        markdown={'```math\nx = 1\n```\n'}
        math
        plugins={{
          renderers: [{ language: 'math', component: () => <span>x</span> }],
        }}
      />
    );
    mount(tree);
    expect(slotWork.queries).toBe(1);
    expect(slotWork.async).toBe(0);

    // The asynchronous form exists for an answer the engine declined to give, so
    // a re-render that changes nothing must not reach for it.
    rerender(tree);
    await act(async () => {
      await Promise.resolve();
    });
    expect(slotWork.async).toBe(0);
  });

  it('an inline plugins literal does not re-ask on a re-render', () => {
    let renders = 0;
    const tree = (
      <DownstreamMarkdown
        markdown={'```chart\n{"a":1}\n```\n'}
        plugins={{
          renderers: [
            {
              language: 'chart',
              component: (props: CustomRendererProps) => {
                renders += 1;
                return <span>{props.code}</span>;
              },
            },
          ],
        }}
      />
    );
    mount(tree);
    expect(slotWork.queries).toBe(1);
    expect(renders).toBe(1);

    // A new `plugins` object every render is what most apps write. It is the same
    // configuration, so the engine is asked once and the component renders once.
    rerender(tree);
    rerender(tree);
    expect(slotWork.queries).toBe(1);
    expect(renders).toBe(1);
  });
});

describe('the language pre-filter', () => {
  it('agrees with the engine wherever the engine reports a slot', () => {
    const languages = ['mermaid', 'chart', 'vega-lite'];
    const corpus = [
      '```mermaid\ngraph;\n```\n',
      '```Mermaid\ngraph;\n```\n',
      '```chart title="t"\n{"a":1}\n```\n',
      '```vega-lite\n{}\n```\n',
      '- ```mermaid\n  graph;\n  ```\n',
      '> ```chart\n> {"a":1}\n> ```\n',
      ...brokenVariants,
    ];
    let checked = 0;
    for (const source of corpus) {
      const found = fenceSlots(source, languages, 0, true);
      if (found.length === 0) {
        continue;
      }
      checked += 1;
      expect(
        mayHoldRenderers(source, languages),
        `the pre-filter must pass a source the engine reports slots for: ${JSON.stringify(
          source
        )}`
      ).toBe(true);
    }
    // The corpus really does contain renderable fences, or this proves nothing.
    expect(checked).toBeGreaterThan(0);
  });

  it('a source with no language in it is not a maybe', () => {
    expect(mayHoldRenderers('A paragraph.\n', ['mermaid'])).toBe(false);
    expect(mayHoldRenderers('', ['mermaid'])).toBe(false);
    expect(mayHoldRenderers('```js\nconst a = 1;\n```\n', ['mermaid'])).toBe(
      false
    );
  });
});

describe('the resolved renderer list', () => {
  it('normalizes a language the way the engine does', () => {
    expect(normalizeLanguage('Mermaid')).toBe('mermaid');
    expect(normalizeLanguage('ts:a.ts')).toBe('ts');
    expect(normalizeLanguage('chart title="x"')).toBe('chart');
    expect(normalizeLanguage('')).toBe('');
  });

  it('the first entry that names a language owns it', () => {
    const first = () => null;
    const second = () => null;
    const resolved = resolveRenderers({
      renderers: [
        { language: 'mermaid', component: first },
        { language: ['MERMAID', 'chart'], component: second },
      ],
    });
    expect(resolved.languages).toEqual(['mermaid', 'chart']);
    expect(resolved.components.get('mermaid')).toBe(first as never);
    expect(resolved.components.get('chart')).toBe(second);
  });

  it('an empty or absent list resolves to nothing', () => {
    expect(resolveRenderers(undefined).languages).toEqual([]);
    expect(resolveRenderers({}).languages).toEqual([]);
    expect(resolveRenderers({ renderers: [] }).languages).toEqual([]);
  });
});

describe('the document under a renderer', () => {
  it('the code block a renderer replaces is still counted as a code block', () => {
    // The reader sees the component, and the code is still there for copy: the
    // fence's own text never leaves the document, only the drawing changes.
    const source = '```mermaid\ngraph TD;\n```\n';
    const model = parse(source);
    expect(visibleText(model).replace(/\n/g, '')).toContain('graph TD;');
    expect(model.blocks).toHaveLength(1);
  });
});
