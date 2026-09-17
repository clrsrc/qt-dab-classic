#!/usr/bin/env python3
"""Erzeugt crates/dab-app/data/autobahnen.bin: deutsche Autobahnen und
Anschlussstellen aus OpenStreetMap fuer die Strassenzuordnung der TPEG-
Verkehrsmeldungen (dab-app::tpeg::roads).

Quelle: OpenStreetMap (Overpass API), Lizenz ODbL - Quellenhinweis
"(c) OpenStreetMap-Mitwirkende" in App/README ist Pflicht.

    python tools/build-autobahnen.py                # laedt per Overpass (Cache in tools/osm-cache/)
    python tools/build-autobahnen.py --offline      # nur aus dem Cache

Dateiformat (little-endian), siehe roads.rs:
    "DABAUTO1"
    u32 n_roads;     je: u8 len, UTF-8 (z. B. "A 3")
    u32 n_ways;      je: u16 road, u16 n_pts, n_pts x (i32 lat_e6, i32 lon_e6) in Fahrtrichtung
    u32 n_junctions; je: u16 road, i32 lat_e6, i32 lon_e6, u8 len name, UTF-8, u8 len ref, UTF-8
    u32 stichtag (JJJJMMTT der OSM-Daten)
Geometrien werden mit Douglas-Peucker (15 m) vereinfacht.
"""
import json, math, os, struct, sys, time, urllib.parse, urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CACHE = os.path.join(ROOT, "tools", "osm-cache")
OUT = os.path.join(ROOT, "crates", "dab-app", "data", "autobahnen.bin")
MIRRORS = [
    "https://overpass.kumi.systems/api/interpreter",
    "https://overpass-api.de/api/interpreter",
    "https://overpass.private.coffee/api/interpreter",
]
Q_WAYS = '[out:json][timeout:900];area(3600051477)->.de;way["highway"="motorway"]["ref"](area.de);out geom qt;'
Q_JUNCTIONS = '[out:json][timeout:300];area(3600051477)->.de;node["highway"="motorway_junction"](area.de);out body;'
TOL_M = 15.0


def fetch(name, query, offline):
    path = os.path.join(CACHE, name)
    if os.path.exists(path):
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    if offline:
        raise SystemExit(f"Cache fehlt: {path}")
    os.makedirs(CACHE, exist_ok=True)
    data = urllib.parse.urlencode({"data": query}).encode()
    for attempt in range(6):
        for url in MIRRORS:
            try:
                req = urllib.request.Request(url, data=data, headers={"User-Agent": "DAB-Classic-dev/1.0"})
                with urllib.request.urlopen(req, timeout=1200) as r:
                    raw = r.read()
                if raw[:200].lstrip().startswith(b"{"):
                    with open(path, "wb") as f:
                        f.write(raw)
                    return json.loads(raw)
                print(f"  {url}: keine JSON-Antwort (Server ausgelastet?)")
            except Exception as e:  # noqa: BLE001
                print(f"  {url}: {e}")
            time.sleep(20)
    raise SystemExit("Overpass nicht erreichbar")


def simplify(pts, tol_deg_lat):
    """Douglas-Peucker (iterativ) auf (lat, lon) in Grad; lon skaliert mit cos(lat)."""
    if len(pts) <= 2:
        return pts
    k = math.cos(math.radians(pts[0][0]))
    keep = [False] * len(pts)
    keep[0] = keep[-1] = True
    stack = [(0, len(pts) - 1)]
    while stack:
        a, b = stack.pop()
        ax, ay = pts[a][1] * k, pts[a][0]
        bx, by = pts[b][1] * k, pts[b][0]
        dx, dy = bx - ax, by - ay
        L2 = dx * dx + dy * dy
        best, bi = 0.0, -1
        for i in range(a + 1, b):
            px, py = pts[i][1] * k, pts[i][0]
            if L2 == 0:
                d = math.hypot(px - ax, py - ay)
            else:
                t = max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / L2))
                d = math.hypot(px - (ax + t * dx), py - (ay + t * dy))
            if d > best:
                best, bi = d, i
        if best > tol_deg_lat and bi >= 0:
            keep[bi] = True
            stack.append((a, bi))
            stack.append((bi, b))
    return [p for p, kf in zip(pts, keep) if kf]


def norm_ref(ref):
    # "A3" -> "A 3", "A 3;A 4" bleibt (gemeinsamer Abschnitt)
    parts = []
    for r in ref.split(";"):
        r = r.strip().replace("  ", " ")
        if len(r) > 1 and r[0] == "A" and r[1].isdigit():
            r = "A " + r[1:]
        parts.append(r)
    return ";".join(parts)


def main():
    offline = "--offline" in sys.argv
    print("== Autobahnen (Overpass)")
    ways_json = fetch("motorways.json", Q_WAYS, offline)
    print("== Anschlussstellen (Overpass)")
    junc_json = fetch("motorway_junctions.json", Q_JUNCTIONS, offline)
    stamp = ways_json.get("osm3s", {}).get("timestamp_osm_base", "")[:10].replace("-", "") or time.strftime("%Y%m%d")

    roads = {}
    ways = []
    npts_raw = 0
    for w in ways_json["elements"]:
        if w.get("type") != "way" or "geometry" not in w:
            continue
        ref = norm_ref(w["tags"].get("ref", ""))
        if not ref:
            continue
        pts = [(g["lat"], g["lon"]) for g in w["geometry"]]
        npts_raw += len(pts)
        pts = simplify(pts, TOL_M / 111_320.0)
        if len(pts) < 2:
            continue
        rid = roads.setdefault(ref, len(roads))
        # in Fahrtrichtung: OSM-Autobahnen sind oneway in Zeichenrichtung; oneway=-1 umdrehen
        if w["tags"].get("oneway") == "-1":
            pts.reverse()
        ways.append((rid, pts))
    road_list = [None] * len(roads)
    for ref, rid in roads.items():
        road_list[rid] = ref
    print(f"   {len(ways)} Wege, {len(roads)} Refs, {npts_raw} -> {sum(len(p) for _, p in ways)} Punkte")

    # Gitter fuer die Zuordnung der Anschlussstellen zur naechsten Autobahn
    cell = 0.02
    grid = {}
    for wi, (rid, pts) in enumerate(ways):
        for si in range(len(pts) - 1):
            for p in (pts[si], pts[si + 1]):
                grid.setdefault((int(p[0] // cell), int(p[1] // cell)), []).append((wi, si))

    def seg_dist_m(p, a, b):
        k = math.cos(math.radians(p[0]))
        ax, ay = (a[1] - p[1]) * k, a[0] - p[0]
        bx, by = (b[1] - p[1]) * k, b[0] - p[0]
        dx, dy = bx - ax, by - ay
        L2 = dx * dx + dy * dy
        t = 0.0 if L2 == 0 else max(0.0, min(1.0, -(ax * dx + ay * dy) / L2))
        return math.hypot(ax + t * dx, ay + t * dy) * 111_320.0

    junctions = []
    skipped = 0
    for n in junc_json["elements"]:
        if n.get("type") != "node":
            continue
        tags = n.get("tags", {})
        name = tags.get("name", "").strip()
        if not name:
            skipped += 1
            continue
        p = (n["lat"], n["lon"])
        cx, cy = int(p[0] // cell), int(p[1] // cell)
        best, brid = 1e9, None
        seen = set()
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                for wi, si in grid.get((cx + dx, cy + dy), ()):
                    if (wi, si) in seen:
                        continue
                    seen.add((wi, si))
                    d = seg_dist_m(p, ways[wi][1][si], ways[wi][1][si + 1])
                    if d < best:
                        best, brid = d, ways[wi][0]
        if brid is None or best > 60.0:
            skipped += 1
            continue
        junctions.append((brid, p, name, tags.get("ref", "").strip()))
    print(f"   {len(junctions)} Anschlussstellen zugeordnet, {skipped} ohne Namen/Autobahn")

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "wb") as f:
        f.write(b"DABAUTO1")
        f.write(struct.pack("<I", len(road_list)))
        for r in road_list:
            b = r.encode("utf-8")[:255]
            f.write(struct.pack("<B", len(b)) + b)
        f.write(struct.pack("<I", len(ways)))
        for rid, pts in ways:
            f.write(struct.pack("<HH", rid, len(pts)))
            for lat, lon in pts:
                f.write(struct.pack("<ii", round(lat * 1e6), round(lon * 1e6)))
        f.write(struct.pack("<I", len(junctions)))
        for rid, (lat, lon), name, ref in junctions:
            nb = name.encode("utf-8")[:255]
            rb = ref.encode("utf-8")[:255]
            f.write(struct.pack("<Hii", rid, round(lat * 1e6), round(lon * 1e6)))
            f.write(struct.pack("<B", len(nb)) + nb + struct.pack("<B", len(rb)) + rb)
        f.write(struct.pack("<I", int(stamp)))
    print(f"== {OUT}: {os.path.getsize(OUT) / 1024:.0f} KB, OSM-Stand {stamp}")


if __name__ == "__main__":
    main()
