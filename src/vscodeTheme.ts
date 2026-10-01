/**
 * A VS Code or Shiki theme's JSON, as `shikiTheme` takes it.
 *
 * Structural rather than exhaustive: only the two fields the converter reads are
 * described, and an index signature lets a whole theme object be passed without
 * naming every key it carries.
 */
export interface VSCodeThemeJson {
  /** Ignored. The converter reads the scopes, not the name. */
  name?: string;
  type?: string;
  /** Ignored by the converter, but VS Code themes carry it. */
  colors?: Readonly<Record<string, string>>;
  tokenColors?: ReadonlyArray<VSCodeTokenColor>;
  readonly [key: string]: unknown;
}

/** One entry of a VS Code theme's `tokenColors`. */
export interface VSCodeTokenColor {
  /**
   * A scope, or several. A TextMate theme matches the most specific prefix, so
   * a scope that is a prefix of another covers it too. A malformed theme with
   * two scopes in one string rather than an array is tolerated, because GitHub's
   * own themes ship three such entries.
   */
  scope: string | ReadonlyArray<string>;
  settings?: Readonly<{
    /** A hex color. Absent on an entry that only sets a font style. */
    foreground?: string;
    background?: string;
    fontStyle?: string;
  }>;
}
