import type { ComponentProps } from 'react';
import type { Ionicons } from '@expo/vector-icons';

export type RouteId =
  | 'example'
  | 'theme'
  | 'controls'
  | 'testing'
  | 'preview'
  | 'bench'
  | 'stress';

export interface Route {
  id: RouteId;
  /** Drawer and app bar label. */
  label: string;
  /** Large title on the screen itself. */
  title: string;
  /** One line explaining what the screen is for. */
  summary: string;
  icon: ComponentProps<typeof Ionicons>['name'];
}

/**
 * Order is the drawer's reading order: the everyday screen first, then the
 * screens that compare and measure, then the one that is only useful when
 * something is already wrong.
 */
export const ROUTES: readonly Route[] = [
  {
    id: 'example',
    label: 'Example',
    title: 'Example',
    summary: 'Stream a Markdown answer from a live model.',
    icon: 'chatbubbles-outline',
  },
  {
    id: 'theme',
    label: 'Theme',
    title: 'Theme',
    summary: 'Slot-based theming, both presets, and a VS Code theme read in.',
    icon: 'color-palette-outline',
  },
  {
    id: 'controls',
    label: 'Controls',
    title: 'Controls',
    summary: 'Copy, download, and wrap on code blocks and tables.',
    icon: 'clipboard-outline',
  },
  {
    id: 'testing',
    label: 'Testing',
    title: 'Streamdown vs Downstream',
    summary: 'Replay one source through both renderers and compare.',
    icon: 'git-compare-outline',
  },
  {
    id: 'preview',
    label: 'Preview',
    title: 'Provisional styling',
    summary: 'Half-written spans against Streamdown, held mid-stream.',
    icon: 'eye-outline',
  },
  {
    id: 'bench',
    label: 'Bench',
    title: 'Playback bench',
    summary: 'Replay a source on a fixed schedule and export a trace.',
    icon: 'stats-chart-outline',
  },
  {
    id: 'stress',
    label: 'Stress',
    title: 'Concurrent stress',
    summary: 'Mount a history, then stream four answers at once.',
    icon: 'flash-outline',
  },
] as const;

export function routeById(id: RouteId): Route {
  return ROUTES.find((route) => route.id === id) ?? ROUTES[0]!;
}
