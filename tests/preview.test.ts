/* eslint-disable no-bitwise -- preview features are a bitmask */
/**
 * The JavaScript half of the provisional-styling pass: the option a caller
 * passes, the call that reaches the native session, and what a reader sees.
 *
 * `tests/real-engine-module.ts` replays each session through the real
 * `IncrementalParser`, in the mode the stream was created with, so these assert
 * on the document the engine would publish rather than on a mock's call log or
 * on the double's own copy of the finish rule. The pass itself is covered in
 * C++ by src/cpp/tests/test_preview.cpp; what matters here is that the option
 * reaches it, that the default stays off, and that both modes agree.
 */
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

vi.mock(
  '../src/NativeDownstreamSession',
  async () => (await import('./real-engine-module')).realEngineModule
);

import {
  createMarkdownStream,
  createMarkdownStreamWithMode,
  getStreamState,
} from '../src/createMarkdownStream';
import type {
  MarkdownPreviewOptions,
  MarkdownStreamController,
} from '../src/types';
import {
  PREVIEW_ALL,
  PREVIEW_EMPHASIS,
  PREVIEW_INLINE_CODE,
  preview,
  stylesOf,
  STYLE,
  visibleText,
} from './engine';
import {
  previewFeaturesOf,
  realEngineModule,
  resetEngine,
  snapshotOf,
} from './real-engine-module';

/**
 * `append` batches onto a frame, so the tests drive the frame queue the way the
 * other stream suites do rather than waiting on a real one.
 */
let frames: FrameRequestCallback[] = [];
const live: MarkdownStreamController[] = [];

function flushFrames(): void {
  const pending = frames;
  frames = [];
  for (const callback of pending) callback(0);
}

/** The text a reader would currently see for a session. */
function shown(sessionId: string): string {
  return visibleText(snapshotOf(sessionId) ?? { blocks: [], links: [] });
}

type Harness = {
  stream: MarkdownStreamController;
  sessionId: string;
  /** Appends through the real frame queue and returns what is now on screen. */
  append(...chunks: string[]): string;
};

function stream(
  options?: {
    preview?: boolean | MarkdownPreviewOptions;
  },
  mode?: 'full' | 'incremental'
): Harness {
  const controller = mode
    ? createMarkdownStreamWithMode(options ?? {}, mode)
    : createMarkdownStream(options);
  live.push(controller);
  const sessionId = getStreamState(controller).sessionId;
  return {
    stream: controller,
    sessionId,
    append(...chunks: string[]): string {
      for (const chunk of chunks) controller.append(chunk);
      flushFrames();
      return shown(sessionId);
    },
  };
}

beforeEach(() => {
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
  for (const controller of live.splice(0)) controller.dispose();
  vi.restoreAllMocks();
});

describe('provisional styling is off by default', () => {
  it('leaves an open span literal when nothing asked for it', () => {
    // The invariant the whole feature exists to preserve. The
    // streaming-preview suite pins the same thing against the raw parser.
    expect(stream().append('Hello **bold wor')).toBe('Hello **bold wor');
  });

  it('reports no features for a stream that was never configured', () => {
    const { stream: controller, sessionId } = stream();
    expect(previewFeaturesOf(sessionId)).toBe(0);
    controller.dispose();
  });

  it('treats false and an empty object as off', () => {
    for (const setting of [false, {}] as const) {
      const { stream: controller, sessionId } = stream({ preview: setting });
      expect(previewFeaturesOf(sessionId)).toBe(0);
      controller.dispose();
    }
  });

  it('leaves an open span literal after finishing, even when enabled', () => {
    // finish() is the contract point: the settled document is a parse of the
    // bytes the writer sent.
    const harness = stream({ preview: true });
    expect(harness.append('Hello **bold wor')).toBe('Hello bold wor');
    harness.stream.finish();
    expect(shown(harness.sessionId)).toBe('Hello **bold wor');
  });
});

describe('the stream option reaches the native session', () => {
  it('accepts true as every feature', () => {
    const { stream: controller, sessionId } = stream({ preview: true });
    expect(previewFeaturesOf(sessionId)).toBe(PREVIEW_ALL);
    controller.dispose();
  });

  it('accepts an object naming one feature', () => {
    const { stream: controller, sessionId } = stream({
      preview: { emphasis: true },
    });
    expect(previewFeaturesOf(sessionId)).toBe(PREVIEW_EMPHASIS);
    controller.dispose();
  });

  it('passes the bitmask through in a single native call', () => {
    const spy = vi.spyOn(realEngineModule.default, 'setPreview');
    const { stream: controller, sessionId } = stream({
      preview: { inlineCode: true },
    });
    expect(spy).toHaveBeenCalledTimes(1);
    expect(spy).toHaveBeenCalledWith(sessionId, PREVIEW_INLINE_CODE);
    controller.dispose();
    spy.mockRestore();
  });

  it('rejects a value that is neither a boolean nor an object', () => {
    // A typo here would otherwise silently change what a reader sees, which is
    // the one thing this option must never do by accident.
    expect(() =>
      createMarkdownStream({ preview: 'yes' as unknown as boolean })
    ).toThrow();
  });
});

describe('styling an open span', () => {
  it('shows the content styled rather than the markers', () => {
    const harness = stream({ preview: true });
    expect(harness.append('Hello **bold wor')).toBe('Hello bold wor');
    const block = snapshotOf(harness.sessionId)?.blocks[0];
    expect(
      block && stylesOf(block, 'bold wor').some((run) => run[2] & STYLE.strong)
    ).toBe(true);
  });

  it('settles as soon as the closer arrives', () => {
    const harness = stream({ preview: true });
    expect(harness.append('Hello **bold wor** and `code`')).toBe(
      'Hello bold wor and code'
    );
  });

  it('leaves a bare marker literal, because there is nothing to style', () => {
    expect(stream({ preview: true }).append('text ***')).toBe('text ***');
  });

  it('leaves a single tilde alone', () => {
    // md4c's GitHub dialect reads `~x~` as strikethrough, so closing one would
    // invent a span the writer never asked for.
    expect(stream({ preview: true }).append('~hello')).toBe('~hello');
  });

  it('never styles a link target that has not been typed', () => {
    // The partial target is dropped whole, markers and all, so nothing inside
    // it is styled and nothing is tappable.
    const harness = stream({ preview: true });
    expect(harness.append('See [the docs](https://exa**bold')).toBe(
      'See the docs'
    );
    expect(snapshotOf(harness.sessionId)?.links).toEqual([]);
  });
});

describe('unfinished links and images', () => {
  it('shows a link as its text until its own bracket arrives', () => {
    const harness = stream({ preview: true });
    expect(harness.append('See [the docs](https://exa')).toBe('See the docs');
    expect(snapshotOf(harness.sessionId)?.links).toEqual([]);
    expect(harness.append('mple.com) now')).toBe('See the docs now');
    expect(
      snapshotOf(harness.sessionId)?.links.map((link) => link.url)
    ).toEqual(['https://example.com']);
  });

  it('shows link text whose bracket has not closed', () => {
    expect(stream({ preview: true }).append('Check the [documentation')).toBe(
      'Check the documentation'
    );
  });

  it('shows an image as its alt text', () => {
    expect(
      stream({ preview: true }).append('See ![diagram](http://example.com/im')
    ).toBe('See diagram');
  });

  it('leaves prose with a space in the would-be target alone', () => {
    expect(stream({ preview: true }).append('see [note](which is long')).toBe(
      'see [note](which is long'
    );
  });

  it('settles to the literal syntax when the stream finishes first', () => {
    const harness = stream({ preview: true });
    harness.append('See [the docs](https://exa');
    harness.stream.finish();
    expect(shown(harness.sessionId)).toBe('See [the docs](https://exa');
  });
});

describe('the features are independent', () => {
  it('emphasis alone does not close a code span', () => {
    const harness = stream({ preview: { emphasis: true } });
    expect(harness.append('a `code')).toBe('a `code');
  });

  it('inline code alone does not close a bold span', () => {
    const harness = stream({ preview: { inlineCode: true } });
    expect(harness.append('a **bold')).toBe('a **bold');
  });

  it('links alone settle a link and nothing else', () => {
    const harness = stream({ preview: { links: true } });
    expect(harness.append('**bold [the docs](https://exa')).toBe(
      '**bold the docs'
    );
  });

  it('images alone leave a link as typed', () => {
    const harness = stream({ preview: { images: true } });
    expect(harness.append('See [the docs](https://exa')).toBe(
      'See [the docs](https://exa'
    );
  });

  it('the inline-code feature styles the span as code', () => {
    const model = preview('a `code', PREVIEW_INLINE_CODE);
    expect(visibleText(model)).toBe('a code');
    expect(
      stylesOf(model.blocks[0]!, 'code').some((run) => run[2] & STYLE.code)
    ).toBe(true);
  });
});

describe('changing the pass mid-stream', () => {
  it('restyles the current tail when turned on later', () => {
    const harness = stream();
    expect(harness.append('Hello **bold wor')).toBe('Hello **bold wor');
    // No new input, so this proves the change reaches the reader on its own
    // rather than riding along on the next append.
    harness.stream.setPreview(true);
    expect(shown(harness.sessionId)).toBe('Hello bold wor');
  });

  it('goes back to literal when turned off', () => {
    const harness = stream({ preview: true });
    expect(harness.append('Hello **bold wor')).toBe('Hello bold wor');
    harness.stream.setPreview(false);
    expect(shown(harness.sessionId)).toBe('Hello **bold wor');
  });

  it('does not call native again when the policy has not changed', () => {
    const { stream: controller } = stream({ preview: true });
    // Spied after construction, so only the explicit calls are counted.
    const spy = vi.spyOn(realEngineModule.default, 'setPreview');
    controller.setPreview(true);
    controller.setPreview({
      emphasis: true,
      inlineCode: true,
      links: true,
      images: true,
    });
    expect(spy).not.toHaveBeenCalled();
    spy.mockRestore();
  });

  it('rejects a call after dispose', () => {
    const { stream: controller } = stream();
    controller.dispose();
    expect(() => controller.setPreview(true)).toThrow();
  });
});

describe('the pass never touches the canonical source', () => {
  it('getSource still returns exactly what was appended', async () => {
    const harness = stream({ preview: true });
    for (const chunk of ['Hello ', '**bold ', 'wor', '** and `code']) {
      harness.stream.append(chunk);
    }
    flushFrames();
    harness.stream.finish();
    await expect(harness.stream.getSource()).resolves.toBe(
      'Hello **bold wor** and `code'
    );
  });

  it('a finished stream matches a parse of the source with no repair', () => {
    const source = 'Hello **bold wor** and `code`';
    const harness = stream({ preview: true });
    harness.stream.append(source);
    flushFrames();
    harness.stream.finish();
    expect(shown(harness.sessionId)).toBe(visibleText(preview(source, 0)));
  });
});

describe('both parse modes', () => {
  for (const mode of ['full', 'incremental'] as const) {
    describe(mode, () => {
      it('previews an open span and drops it on finish', () => {
        const harness = stream({ preview: true }, mode);
        expect(harness.append('Hello **bold wor')).toBe('Hello bold wor');
        harness.stream.finish();
        expect(shown(harness.sessionId)).toBe('Hello **bold wor');
      });

      it('keeps previewing across a chunk that ends on a space', () => {
        const harness = stream({ preview: true }, mode);
        expect(harness.append('Hello **bold')).toBe('Hello bold');
        expect(harness.append(' ')).toBe('Hello bold');
        expect(harness.append('word')).toBe('Hello bold word');
      });

      it('closes nested spans together', () => {
        const harness = stream({ preview: true }, mode);
        expect(harness.append('**bold *ital')).toBe('bold ital');
        const block = snapshotOf(harness.sessionId)?.blocks[0];
        const styles = block
          ? stylesOf(block, 'ital').reduce((mask, run) => mask | run[2], 0)
          : 0;
        expect(styles & STYLE.strong).toBeTruthy();
        expect(styles & STYLE.emphasis).toBeTruthy();
      });

      it('leaves a heading literal once its line has ended', () => {
        const harness = stream({ preview: true }, mode);
        expect(harness.append('# **Tit')).toBe('Tit');
        expect(harness.append('le\n')).toBe('**Title');
      });

      it('keeps previewing after a reset to a new source', () => {
        const harness = stream({ preview: true }, mode);
        harness.append('first');
        harness.stream.reset('Hello **bold wor');
        flushFrames();
        expect(shown(harness.sessionId)).toBe('Hello bold wor');
      });
    });
  }
});
