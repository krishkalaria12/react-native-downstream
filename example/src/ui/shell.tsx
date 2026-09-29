import {
  createContext,
  useContext,
  useMemo,
  useState,
  type ReactNode,
} from 'react';

interface Shell {
  open: boolean;
  setOpen: (open: boolean) => void;
  toggle: () => void;
}

const ShellContext = createContext<Shell>({
  open: false,
  setOpen: () => {},
  toggle: () => {},
});

/** Owns drawer state so any screen can open it from its app bar. */
export function ShellProvider({ children }: { children: ReactNode }) {
  const [open, setOpen] = useState(false);
  const value = useMemo<Shell>(
    () => ({ open, setOpen, toggle: () => setOpen((current) => !current) }),
    [open]
  );
  return (
    <ShellContext.Provider value={value}>{children}</ShellContext.Provider>
  );
}

export function useShell(): Shell {
  return useContext(ShellContext);
}
