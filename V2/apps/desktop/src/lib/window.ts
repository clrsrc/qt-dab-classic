// Fenstersteuerung des rahmenlosen Hauptfensters. Einziger Ort ausserhalb
// core.ts mit Tauri-Importen (Entscheidung 23).

import { getCurrentWindow } from "@tauri-apps/api/window";

export const win = {
  minimize: () => getCurrentWindow().minimize(),
  close: () => getCurrentWindow().close(),
  startDragging: () => getCurrentWindow().startDragging(),
};
