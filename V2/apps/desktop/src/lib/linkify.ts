// Radiotext (DLS/DL+) kann URLs enthalten (Bugfixes.txt #9); zerlegt einen
// Text in klickbare und normale Abschnitte, statt ihn als reinen Text-Node zu rendern.

const URL_RE = /(https?:\/\/[^\s<>"']+|www\.[^\s<>"']+)/gi;

export interface TextSegment {
  text: string;
  url?: string;
}

export function linkify(text: string): TextSegment[] {
  if (!text) return [];
  const segments: TextSegment[] = [];
  let last = 0;
  for (const m of text.matchAll(URL_RE)) {
    const start = m.index ?? 0;
    if (start > last) segments.push({ text: text.slice(last, start) });
    const raw = m[0].replace(/[),.;!?]+$/, ""); // Satzzeichen am Ende nicht mit in den Link ziehen
    segments.push({ text: raw, url: raw.startsWith("www.") ? `https://${raw}` : raw });
    last = start + raw.length;
  }
  if (last < text.length) segments.push({ text: text.slice(last) });
  return segments;
}
