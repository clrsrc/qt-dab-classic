// Dienst-Metadaten aus dem FIC (17.09.2026): Programmtyp (FIG 0/17, TS 101 756
// Tabelle 12), Sprache (FIG 0/5, Tabellen 9/10) und Kurzlabel (FIG 1 Zeichen-
// Flags). Die Namen liegen als i18n-Schluessel "pty.<n>" / "lang.<hex>" vor;
// fehlende Schluessel fallen auf den rohen Code zurueck.
import { t } from "$lib/i18n.svelte";
import type { ServiceInfo } from "$lib/core";

/** Programmtyp-Name; "" fuer 0 (kein Programmtyp) oder unbelegte Codes. */
export function ptyName(pty: number): string {
  if (!pty || pty > 29) return "";
  const key = `pty.${pty}`;
  const s = t(key);
  return s === key ? `PTy ${pty}` : s;
}

/** Sprachname; "" fuer 0 (unbekannt). */
export function languageName(code: number): string {
  if (!code) return "";
  const key = `lang.${code.toString(16).toUpperCase().padStart(2, "0")}`;
  const s = t(key);
  return s === key ? `0x${code.toString(16).toUpperCase().padStart(2, "0")}` : s;
}

/** Kurzlabel fuer die Speichertasten: Kurzlabel des Senders, sonst der volle Name. */
export function shortLabel(x: { name: string; short_name?: string }, useShort: boolean): string {
  const short = (x.short_name ?? "").trim();
  return useShort && short ? short : x.name.trim();
}

/** Tooltip-Zeile "Genre · Sprache" fuer eine Dienstzeile. */
export function metaTooltip(x: { pty: number; language: number; short_name?: string }): string {
  const parts = [ptyName(x.pty), languageName(x.language)].filter(Boolean);
  const short = (x.short_name ?? "").trim();
  if (short) parts.push(`„${short}“`);
  return parts.join(" · ");
}

/** Vorhandene Programmtypen einer Liste, sortiert nach Name (fuer den Genre-Filter). */
export function ptyOptions(list: { pty: number; is_audio: boolean }[]): { pty: number; label: string }[] {
  const seen = new Set<number>();
  for (const x of list) if (x.is_audio && x.pty && x.pty <= 29) seen.add(x.pty);
  return Array.from(seen)
    .map((pty) => ({ pty, label: ptyName(pty) }))
    .sort((a, b) => a.label.localeCompare(b.label));
}

export const matchesPty = (x: { pty: number }, filter: number) => filter === 0 || x.pty === filter;

export type { ServiceInfo };
