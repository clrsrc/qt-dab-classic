#!/usr/bin/env python3
"""Spike 3 – DL+-Verfuegbarkeit je Audiodienst eines Mitschnitts.

Laesst dabcored einmal ueber einen Ausschnitt laufen und dekodiert dabei alle
Audiodienste des Ensembles gleichzeitig (Background-Slots, --all-audio), zaehlt
je Dienst dls / dl_plus, die vorkommenden DL+-Content-Types, IT-Wechsel und
IR-Anteil und schreibt eine Markdown-Tabelle.

    python tools/dlplus-stats.py DATEI.uff [--duration 300] [--out docs/Spike3.md]
                                 [--events out.jsonl] [--core PFAD]
"""
import argparse
import json
import os
import subprocess
import sys
import time
from collections import Counter, OrderedDict, defaultdict

CT_NAMES = {
    0: "DUMMY", 1: "ITEM.TITLE", 2: "ITEM.ALBUM", 3: "ITEM.TRACKNUMBER", 4: "ITEM.ARTIST",
    5: "ITEM.COMPOSITION", 6: "ITEM.MOVEMENT", 7: "ITEM.CONDUCTOR", 8: "ITEM.COMPOSER",
    9: "ITEM.BAND", 10: "ITEM.COMMENT", 11: "ITEM.GENRE", 12: "INFO.NEWS", 13: "INFO.NEWS.LOCAL",
    14: "INFO.STOCKMARKET", 15: "INFO.SPORT", 16: "INFO.LOTTERY", 17: "INFO.HOROSCOPE",
    18: "INFO.DAILY_DIVERSION", 19: "INFO.HEALTH", 20: "INFO.EVENT", 21: "INFO.SCENE",
    22: "INFO.CINEMA", 23: "INFO.TV", 24: "INFO.DATE_TIME", 25: "INFO.WEATHER", 26: "INFO.TRAFFIC",
    27: "INFO.ALARM", 28: "INFO.ADVERTISEMENT", 29: "INFO.URL", 30: "INFO.OTHER",
    31: "STATIONNAME.SHORT", 32: "STATIONNAME.LONG", 33: "PROGRAMME.NOW", 34: "PROGRAMME.NEXT",
    35: "PROGRAMME.PART", 36: "PROGRAMME.HOST", 37: "PROGRAMME.EDITORIAL_STAFF",
    38: "PROGRAMME.FREQUENCY", 39: "PROGRAMME.HOMEPAGE", 40: "PROGRAMME.SUBCHANNEL",
    41: "PHONE.HOTLINE", 42: "PHONE.STUDIO", 43: "PHONE.OTHER", 44: "SMS.STUDIO", 45: "SMS.OTHER",
    46: "EMAIL.HOTLINE", 47: "EMAIL.STUDIO", 48: "EMAIL.OTHER", 49: "MMS.OTHER", 50: "CHAT",
    51: "CHAT.CENTER", 52: "VOTE.QUESTION", 53: "VOTE.CENTRE", 56: "PRIVATE_1", 57: "PRIVATE_2",
    58: "PRIVATE_3", 59: "DESCRIPTOR.PLACE", 60: "DESCRIPTOR.APPOINTMENT", 61: "DESCRIPTOR.IDENTIFIER",
    62: "DESCRIPTOR.PURCHASE", 63: "DESCRIPTOR.GET_DATA",
}


def find_core(explicit):
    here = os.path.dirname(os.path.abspath(__file__))
    cands = [explicit,
             os.path.join(here, "..", "core-cpp", "build", "dabcored.exe"),
             os.path.join(here, "..", "apps", "desktop", "src-tauri", "resources", "core", "dabcored.exe")]
    for c in cands:
        if c and os.path.exists(c):
            return os.path.abspath(c)
    sys.exit("dabcored.exe nicht gefunden (--core)")


def run_core(exe, path, duration, events):
    env = dict(os.environ)
    env["PATH"] = r"C:\msys64\ucrt64\bin;" + env.get("PATH", "")
    args = [exe, "--no-audio", "--fast", "--all-audio", "--file", path, "--events", events]
    if duration:
        args += ["--duration", str(duration)]
    t0 = time.time()
    # stdin offen halten (EOF wuerde den Kern beenden); der Prozess endet am
    # (--duration-)Dateiende. Ereignisse gehen in die Datei, stderr in eine
    # Datei daneben.
    errpath = events + ".stderr"
    with open(errpath, "wb") as errf:
        p = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=errf,
                             cwd=os.path.dirname(exe), env=env)
        p.wait()
        p.stdin.close()
    dt = time.time() - t0
    if p.returncode != 0:
        print(open(errpath, encoding="utf-8", errors="replace").read(), file=sys.stderr)
        sys.exit("dabcored Exit %d" % p.returncode)
    return dt


def analyse(events_path):
    services = OrderedDict()   # sid -> info
    stats = defaultdict(lambda: {
        "dls": 0, "dls_texts": [], "dlplus": 0, "ct": Counter(), "it_changes": 0, "ir_true": 0,
        "last_it": None, "titles": [], "artists": [], "started": None, "stats": 0,
        "frame_errors": 0, "aac_errors": 0, "examples": {}})
    first_ts = None
    with open(events_path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            ev = json.loads(line)
            t = ev.get("type")
            if t == "service_added":
                s = ev["service"]
                if s["is_audio"] and s["sid"] not in services:
                    services[s["sid"]] = s
            elif t == "service_started":
                stats[ev["sid"]]["started"] = ev.get("codec")
            elif t == "dls":
                st = stats[ev["sid"]]
                st["dls"] += 1
                if ev["text"] not in st["dls_texts"]:
                    st["dls_texts"].append(ev["text"])
            elif t == "dl_plus":
                st = stats[ev["sid"]]
                st["dlplus"] += 1
                if ev["item_running"]:
                    st["ir_true"] += 1
                it = ev["item_toggle"]
                if st["last_it"] is not None and it != st["last_it"]:
                    st["it_changes"] += 1
                st["last_it"] = it
                for ct, text in ev["tags"]:
                    st["ct"][ct] += 1
                    if text and ct not in st["examples"]:
                        st["examples"][ct] = text
                    if ct == 1 and text and text not in st["titles"]:
                        st["titles"].append(text)
                    if ct == 4 and text and text not in st["artists"]:
                        st["artists"].append(text)
            elif t == "service_stats":
                st = stats[ev["sid"]]
                st["stats"] += 1
                st["frame_errors"] += ev["frame_errors"]
                st["aac_errors"] += ev["aac_errors"]
    return services, stats


def md_escape(s):
    return s.replace("|", "\\|").replace("\n", " ")


def write_report(out, path, duration, wall, services, stats):
    lines = []
    lines.append("# Spike 3 – DL+-Verfügbarkeit im Bundesmux (5C, Warntag-Mitschnitt)")
    lines.append("")
    lines.append("Werkzeug: `tools/dlplus-stats.py`, Kern `dabcored --fast --all-audio` (alle Audiodienste als")
    lines.append("Background-Slots in einem Durchlauf).  ")
    if wall > 0:
        lines.append("Datei: `%s`, Ausschnitt %s s Dateizeit, Laufzeit %.1f s (Faktor %.1fx Echtzeit).  " %
                     (path, duration or "gesamt", wall, (duration / wall) if duration else 0))
    else:
        lines.append("Datei: `%s`, Ausschnitt %s s Dateizeit (Auswertung vorhandener Ereignisse).  " %
                     (path, duration or "gesamt"))
    lines.append("Stand: %s" % time.strftime("%Y-%m-%d %H:%M"))
    lines.append("")
    lines.append("## Tabelle")
    lines.append("")
    lines.append("| Dienst | SId | kbit/s | Codec | Superframe-Fehler | dls (distinct) | dl_plus | Content-Types | IT-Wechsel | IR-Anteil | Beispiel ITEM.TITLE / ITEM.ARTIST |")
    lines.append("|---|---|---|---|---|---|---|---|---|---|---|")
    for sid, s in services.items():
        st = stats.get(sid)
        if st is None:
            st = stats[sid]
        codec = "-"
        if st["started"]:
            c = st["started"]
            if c.get("codec") == "he_aac":
                codec = "HE-AAC %d k%s%s" % (c["sample_rate"] // 1000, " SBR" if c["sbr"] else "", " PS" if c["ps"] else "")
            else:
                codec = c.get("codec", "-")
        cts = ", ".join("%d %s" % (ct, CT_NAMES.get(ct, "?")) for ct in sorted(st["ct"]))
        ir = ("%d %%" % round(100.0 * st["ir_true"] / st["dlplus"])) if st["dlplus"] else "-"
        ex = ""
        if st["titles"]:
            ex = "„%s“" % st["titles"][0]
            if st["artists"]:
                ex += " / „%s“" % st["artists"][0]
        elif st["dls_texts"]:
            ex = "(DLS) „%s“" % st["dls_texts"][0]
        lines.append("| %s | %04X | %d | %s | %d | %d (%d) | %d | %s | %d | %s | %s |" % (
            md_escape(s["name"]), sid, s["bitrate_kbps"], codec, st["frame_errors"], st["dls"], len(st["dls_texts"]), st["dlplus"],
            cts or "-", st["it_changes"], ir, md_escape(ex)))
    lines.append("")
    lines.append("Spalten: *Superframe-Fehler* = Summe der service_stats.frame_errors (Superframes ohne Firecode-/RS-Erfolg, "
                 "jeder kostet 120 ms Audio und PAD, daher abgeschnittene DLS-Fragmente); "
                 "*dls* = Anzahl gemeldeter (geänderter) Labels, in Klammern verschiedene Texte; "
                 "*dl_plus* = Anzahl DL+-Kommandos; *IT-Wechsel* = Wechsel des Item-Toggle-Bits (= Titelwechsel); "
                 "*IR-Anteil* = Anteil der Kommandos mit Item-Running = 1.")
    lines.append("")
    lines.append("## Beispiele je Dienst")
    lines.append("")
    for sid, s in services.items():
        st = stats.get(sid)
        if not st or (not st["dls_texts"] and not st["dlplus"]):
            continue
        lines.append("### %s (%04X)" % (s["name"], sid))
        lines.append("")
        for t in st["dls_texts"][:6]:
            lines.append("- DLS: %s" % md_escape(t))
        for ct in sorted(st["examples"]):
            lines.append("- DL+ %d %s: %s" % (ct, CT_NAMES.get(ct, "?"), md_escape(st["examples"][ct])))
        if len(st["titles"]) > 1:
            lines.append("- weitere Titel: " + "; ".join(md_escape(t) for t in st["titles"][1:6]))
        lines.append("")
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    print("\n".join(lines[:8 + len(services) + 2]))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("file")
    ap.add_argument("--duration", type=float, default=300.0, help="Dateizeit in s (0 = ganze Datei)")
    ap.add_argument("--out", default=None, help="Markdown-Ausgabe (Standard: docs/Spike3-DLplus-<datum>.md)")
    ap.add_argument("--events", default=None, help="JSONL-Ereignisse behalten (Standard: temporaer)")
    ap.add_argument("--core", default=None)
    ap.add_argument("--reuse", action="store_true", help="vorhandene --events-Datei nur auswerten")
    a = ap.parse_args()
    exe = find_core(a.core)
    here = os.path.dirname(os.path.abspath(__file__))
    out = a.out or os.path.join(here, "..", "docs", "Spike3-DLplus-%s.md" % time.strftime("%Y-%m-%d"))
    events = a.events or os.path.join(os.environ.get("TEMP", "."), "dlplus-stats.jsonl")
    wall = 0.0
    if not (a.reuse and os.path.exists(events)):
        print("dabcored laeuft: %s, %s s ..." % (a.file, a.duration or "gesamt"))
        wall = run_core(exe, a.file, a.duration, events)
        print("fertig nach %.1f s" % wall)
    services, stats = analyse(events)
    write_report(out, a.file, a.duration, wall, services, stats)
    print("-> %s" % os.path.abspath(out))


if __name__ == "__main__":
    main()
