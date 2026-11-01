export const CPP: string;
export const CODE_CPP: string;
export const CODE_SOURCES: string[];
export const ENGINE: string[];
export const MD4C_SOURCES: string[];
export const HIGHLIGHT_SOURCES: string[];
export function compile(
  sources: string[],
  name: string,
  withCode?: boolean
): string;
export function prepareTargets(
  targets: Record<string, string[]>
): Promise<void>;
