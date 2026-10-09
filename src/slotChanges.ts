/** Per-subscriber delta state. Native tokens name documents, independently of input sequence. */
export type SlotChangeState<T> = {
  token: number;
  slots: readonly T[];
};

export function applySlotChanges<T>(
  state: SlotChangeState<T>,
  changes: Readonly<{
    token: number;
    order: readonly number[];
    changed: readonly T[];
  }>,
  key: (slot: T) => number
): readonly T[] {
  const slots = new Map(state.slots.map((slot) => [key(slot), slot]));
  for (const slot of changes.changed) slots.set(key(slot), slot);
  const ordered = changes.order.map((id) => {
    const slot = slots.get(id);
    if (slot === undefined) throw new Error('Missing native slot payload');
    return slot;
  });
  state.token = changes.token;
  state.slots = ordered;
  return ordered;
}
