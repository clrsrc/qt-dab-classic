// TII-Darstellung: Sendername, Entfernung, Azimut/Himmelsrichtung fuer
// TiiList, Statusleiste und Einstellungen. Daten kommen aus `s.tii`
// (dab-app::tii, nach Staerke sortiert).

import type { TiiSeen } from "./debug";
import { s } from "./state.svelte";

const COMPASS = ["N", "NE", "E", "SE", "S", "SW", "W", "NW"];

export function compass(azimuth: number): string {
  const a = ((azimuth % 360) + 360) % 360;
  return COMPASS[Math.floor((a + 22.5) / 45) % 8];
}

/** "(20,4)" – Kennung wie in v1. */
export function tiiId(t: TiiSeen): string {
  return `(${t.main_id},${t.sub_id})`;
}

/** Sendername oder "(mainId,subId)" ohne Datenbanktreffer. */
export function tiiName(t: TiiSeen): string {
  return t.transmitter?.name ?? tiiId(t);
}

export function fmtDistance(km: number | null): string {
  if (km == null || !isFinite(km)) return "";
  return km < 10 ? `${km.toFixed(1)} km` : `${Math.round(km)} km`;
}

export function fmtAzimuth(deg: number | null): string {
  if (deg == null || !isFinite(deg)) return "";
  return `${Math.round(deg)}° ${compass(deg)}`;
}

/** Staerkster Sender (Liste ist sortiert), null ohne TII. */
export function strongestTii(): TiiSeen | null {
  return s.tii.length ? s.tii[0] : null;
}

/** Zeile fuer die Statusleiste: "Langenberg/Hordtberg 30 km NE". */
export function strongestLabel(): string {
  const t = strongestTii();
  if (!t) return "";
  const parts = [tiiName(t)];
  if (t.distance_km != null) parts.push(fmtDistance(t.distance_km));
  if (t.azimuth_deg != null) parts.push(compass(t.azimuth_deg));
  return parts.join(" ");
}

export function hasHome(): boolean {
  return s.tii.some((t) => t.distance_km != null);
}
