#!/bin/bash
# Qt-DAB Portable Deployment Script
# Erstellt einen portablen Ordner mit EXE + DLLs + Qt-Plugins
#
# Verwendung: cd /p/Projekte/DAB && bash deploy.sh
#
# Voraussetzung auf Zielrechner: WinUSB-Treiber fuer HackRF (via Zadig)

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
QT_PLUGINS="/c/msys64/ucrt64/share/qt6/plugins"
DEPLOY_DIR="$SCRIPT_DIR/Qt-DAB-portable"

echo "=== Qt-DAB Portable Deployment ==="
echo ""

# Pruefen ob Build existiert
if [ ! -f "$BUILD_DIR/Qt-DAB.exe" ]; then
    echo "FEHLER: $BUILD_DIR/Qt-DAB.exe nicht gefunden!"
    echo "Zuerst bauen: cd build && ninja -j4"
    exit 1
fi

# Alten Deploy-Ordner loeschen
if [ -d "$DEPLOY_DIR" ]; then
    echo "Loesche alten Deploy-Ordner..."
    rm -rf "$DEPLOY_DIR"
fi

mkdir -p "$DEPLOY_DIR"

# 1. EXE kopieren
echo "[1/6] Qt-DAB.exe kopieren..."
cp "$BUILD_DIR/Qt-DAB.exe" "$DEPLOY_DIR/"

# 2. Alle DLLs aus dem Build-Ordner
echo "[2/6] DLLs kopieren..."
cp "$BUILD_DIR"/*.dll "$DEPLOY_DIR/"

# 3. Qt-Plugins (relative Unterordner zur EXE)
echo "[3/6] Qt-Plugins kopieren..."

# platforms — Pflicht
mkdir -p "$DEPLOY_DIR/platforms"
cp "$QT_PLUGINS/platforms/qwindows.dll" "$DEPLOY_DIR/platforms/"

# styles — natives Windows-Look
mkdir -p "$DEPLOY_DIR/styles"
cp "$QT_PLUGINS/styles/qmodernwindowsstyle.dll" "$DEPLOY_DIR/styles/"

# multimedia — fuer Qt6Multimedia
mkdir -p "$DEPLOY_DIR/multimedia"
cp "$QT_PLUGINS/multimedia/ffmpegmediaplugin.dll" "$DEPLOY_DIR/multimedia/"

# imageformats — fuer MOT Slideshow / Logos
mkdir -p "$DEPLOY_DIR/imageformats"
cp "$QT_PLUGINS/imageformats/qjpeg.dll" "$DEPLOY_DIR/imageformats/"
cp "$QT_PLUGINS/imageformats/qpng16-16.dll" "$DEPLOY_DIR/imageformats/" 2>/dev/null || true
cp "$QT_PLUGINS/imageformats/qsvg.dll" "$DEPLOY_DIR/imageformats/"
cp "$QT_PLUGINS/imageformats/qico.dll" "$DEPLOY_DIR/imageformats/"
cp "$QT_PLUGINS/imageformats/qgif.dll" "$DEPLOY_DIR/imageformats/"

# iconengines — fuer SVG-Icons
mkdir -p "$DEPLOY_DIR/iconengines"
cp "$QT_PLUGINS/iconengines/qsvgicon.dll" "$DEPLOY_DIR/iconengines/"

# tls — fuer Qt6Network (EPG-Downloads etc.)
mkdir -p "$DEPLOY_DIR/tls"
cp "$QT_PLUGINS/tls/qschannelbackend.dll" "$DEPLOY_DIR/tls/"

# 4. Daten-Ordner vorbereiten
echo "[4/6] Daten-Ordner anlegen..."
mkdir -p "$DEPLOY_DIR/data/Qt-DAB-files"
mkdir -p "$DEPLOY_DIR/data/Qt-DAB-recordings"

# 5. Zadig + Anleitung
echo "[5/6] Zadig + Anleitung kopieren..."
ZADIG="/c/Program Files/welle.io/zadig-2.9.exe"
if [ -f "$ZADIG" ]; then
    cp "$ZADIG" "$DEPLOY_DIR/"
else
    echo "  WARNUNG: Zadig nicht gefunden unter $ZADIG"
    echo "  Manuell von https://zadig.akeo.ie herunterladen und in den Ordner legen."
fi
cp "$SCRIPT_DIR/ANLEITUNG.txt" "$DEPLOY_DIR/" 2>/dev/null || \
echo "  WARNUNG: ANLEITUNG.txt nicht gefunden"

# 6. Start-Skript (setzt HOME auf portablen Ordner)
echo "[6/6] Start-Skript erstellen..."
cat > "$DEPLOY_DIR/Qt-DAB-Start.bat" << 'BATCH'
@echo off
:: Qt-DAB Portable Starter
:: Setzt das Home-Verzeichnis auf den portablen Ordner,
:: damit INI, EPG, Aufnahmen und Timer hier gespeichert werden.
set HOME=%~dp0data
set USERPROFILE=%~dp0data
start "" "%~dp0Qt-DAB.exe"
BATCH

# Zusammenfassung
echo ""
echo "=== Fertig! ==="
echo ""

EXE_COUNT=$(find "$DEPLOY_DIR" -name "*.exe" | wc -l)
DLL_COUNT=$(find "$DEPLOY_DIR" -name "*.dll" | wc -l)
TOTAL_SIZE=$(du -sh "$DEPLOY_DIR" | cut -f1)

echo "Deployment: $DEPLOY_DIR"
echo "  $EXE_COUNT EXE, $DLL_COUNT DLLs"
echo "  Gesamtgroesse: $TOTAL_SIZE"
echo ""
echo "Inhalt:"
echo "  Qt-DAB.exe          - Direkt starten (Daten im Home-Verzeichnis)"
echo "  Qt-DAB-Start.bat    - Portabel starten (Daten im data/ Ordner)"
echo "  zadig-2.9.exe       - HackRF USB-Treiber (einmalig)"
echo "  ANLEITUNG.txt       - Ersteinrichtung + Bedienung"
echo "  data/               - EPG, Aufnahmen, Einstellungen (portabel)"
find "$DEPLOY_DIR" -mindepth 1 -maxdepth 1 -type d ! -name "data" | sort | while read dir; do
    reldir="${dir#$DEPLOY_DIR/}"
    count=$(ls "$dir"/*.dll 2>/dev/null | wc -l)
    echo "  $reldir/ ($count Plugins)"
done
echo ""
echo "Portabel-Modus: Qt-DAB-Start.bat nutzen"
echo "  -> Alle Daten werden in data/ gespeichert"
echo "  -> Kein Schreiben ins Windows-Benutzerprofil"
