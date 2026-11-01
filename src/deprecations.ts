const warned = new Set<string>();

/** Report each compatibility alias once per development session. */
export function warnDeprecated(name: string, replacement: string): void {
  if (typeof __DEV__ !== 'boolean' || !__DEV__ || warned.has(name)) return;
  warned.add(name);
  console.warn(
    `[react-native-downstream] ${name} is deprecated. Use ${replacement}.`
  );
}

/** Clears the warning registry for isolated tests. */
export function resetDeprecationWarnings(): void {
  warned.clear();
}
