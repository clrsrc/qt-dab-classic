// Eigene Dialoge (Bestaetigung, Kontextmenue) innerhalb der Seite – kein
// window.confirm, damit Stil und Sprache zur Shell passen.

export interface MenuItem {
  label: string;
  action: () => void;
  disabled?: boolean;
}

export const dialogs = $state({
  confirm: null as null | { title: string; text: string; ok: string; cancel: string; resolve: (v: boolean) => void },
  menu: null as null | { x: number; y: number; items: MenuItem[] },
  /** Offene Vergroesserungen (Logo/SlideShow); Escape gehoert dann ihnen, nicht Timeshift (Befund 5). */
  zoom: 0,
});

/** Ob gerade ein Dialog/Menue offen ist, dem die Tastatur gehoert (Befund 4). */
export function dialogOpen(): boolean {
  return !!dialogs.confirm || !!dialogs.menu;
}

export function confirm(title: string, text: string, ok: string, cancel: string): Promise<boolean> {
  return new Promise((resolve) => {
    dialogs.confirm = { title, text, ok, cancel, resolve };
  });
}

export function closeConfirm(v: boolean) {
  dialogs.confirm?.resolve(v);
  dialogs.confirm = null;
}

export function openMenu(x: number, y: number, items: MenuItem[]) {
  dialogs.menu = { x, y, items };
}

export function closeMenu() {
  dialogs.menu = null;
}
