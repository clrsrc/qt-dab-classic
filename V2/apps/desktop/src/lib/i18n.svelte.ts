// Sprachumschaltung (Entscheidung 27): JSON-Dateien de/en, Systemsprache
// als Vorgabe, manuell umschaltbar. `t()` liest den reaktiven Zustand, damit
// sich alle Texte beim Umschalten sofort aendern.

import de from "./i18n/de.json";
import en from "./i18n/en.json";

export type Lang = "de" | "en";
const tables: Record<Lang, Record<string, string>> = { de, en };

export const i18n = $state<{ lang: Lang }>({ lang: systemLang() });

export function systemLang(): Lang {
  const l = typeof navigator !== "undefined" ? navigator.language : "de";
  return l.toLowerCase().startsWith("de") ? "de" : "en";
}

/** Sprache setzen; `null` = Systemsprache. */
export function setLang(lang: string | null) {
  i18n.lang = lang === "de" || lang === "en" ? lang : systemLang();
}

export function t(key: string, vars?: Record<string, string | number>): string {
  let s = tables[i18n.lang][key] ?? tables.de[key] ?? key;
  if (vars) {
    for (const [k, v] of Object.entries(vars)) {
      s = s.replaceAll(`{${k}}`, String(v));
    }
  }
  return s;
}

/** Fehlertext aus der Rust-Seite uebersetzen (Schluessel "error.<text>"). */
export function tError(e: unknown): string {
  const msg = typeof e === "string" ? e : e instanceof Error ? e.message : String(e);
  const key = `error.${msg}`;
  return tables[i18n.lang][key] ?? tables.de[key] ?? msg;
}
