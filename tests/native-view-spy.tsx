/**
 * Mounts a Downstream component and captures what the native view received.
 *
 * The library renders a Fabric view, not DOM, so there is nothing to query. The
 * native component is replaced with a spy, which makes the whole contract with
 * native observable: the props it was handed, and how many times it committed.
 */
import { act } from 'react';
import { createRoot, type Root } from 'react-dom/client';

export interface NativeViewRecord {
  type: unknown;
  props: Record<string, unknown>;
}

export interface NativeViewSpy {
  /** Renders the replacement native component. */
  component: (props: Record<string, unknown>) => null;
  /** Every commit the native view received, oldest first. */
  calls: NativeViewRecord[];
  /** The most recent commit's props. */
  last(): Record<string, unknown> | undefined;
  /** How many commits reached the native view. */
  count(): number;
  reset(): void;
}

/**
 * Builds a spy for the native component module. Pass it to `vi.mock` so the
 * component under test picks it up.
 */
export function createNativeViewSpy(): NativeViewSpy {
  const calls: NativeViewRecord[] = [];
  return {
    component: (props) => {
      calls.push({ type: 'DownstreamMarkdownView', props });
      return null;
    },
    calls,
    last: () => calls.at(-1)?.props,
    count: () => calls.length,
    reset: () => {
      calls.length = 0;
    },
  };
}

export interface MountedHost {
  /** Rerenders, optionally with a new element, to prove work is skipped. */
  rerender(element?: React.ReactElement): void;
  unmount(): void;
}

let host: HTMLDivElement | undefined;

function mountPoint(): HTMLDivElement {
  host ??= document.createElement('div');
  document.body.append(host);
  return host;
}

export function mount(element: React.ReactElement): MountedHost {
  const root: Root = createRoot(mountPoint());
  act(() => {
    root.render(element);
  });
  let current = element;
  return {
    rerender: (next?: React.ReactElement) => {
      current = next ?? current;
      act(() => {
        root.render(current);
      });
    },
    unmount: () => {
      act(() => {
        root.unmount();
      });
    },
  };
}
