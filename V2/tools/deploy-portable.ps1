# Erzeugt den portablen Ordner fuer DAB Classic (Entscheidung 12):
#   dab-classic.exe + WebView2Loader.dll + core\ (dabcored.exe + DLLs) + tii\ (txdata.tii) + data\
#   + webview2\ (Fixed-Version-Runtime) + zadig + ANLEITUNG.txt.
#   Kein Installer, keine Start.bat: `data\` neben der EXE schaltet den
#   portablen Modus ein (dab-app::paths).
#
#   .\tools\deploy-portable.ps1                 # baut Kern + App (release), packt nach dist\DAB-Classic-portable\
#   .\tools\deploy-portable.ps1 -SkipBuild      # nur packen
#   .\tools\deploy-portable.ps1 -Zip            # zusaetzlich dist\DAB-Classic-v3.0-dev-portable-win64.zip
#   .\tools\deploy-portable.ps1 -WebView2Runtime C:\Downloads\Microsoft.WebView2.FixedVersionRuntime.x64.cab
#
# WebView2: Die Fixed-Version-Runtime (~300 MB .cab, entpackt ~600 MB) kommt von
# https://developer.microsoft.com/microsoft-edge/webview2/#download (Abschnitt
# "Fixed Version", x64). Ohne -WebView2Runtime wird die neueste .cab aus
# third_party\webview2\ genommen (gitignored); fehlt auch die, laeuft die App mit
# der installierten Evergreen-Runtime. wry/WebView2Loader werten
# WEBVIEW2_BROWSER_EXECUTABLE_FOLDER aus; die App setzt die Variable selbst,
# wenn neben der EXE ein Ordner "webview2\" mit msedgewebview2.exe liegt
# (apps/desktop/src-tauri/src/main.rs).
param(
    [switch]$SkipBuild,
    [switch]$Zip,
    [string]$WebView2Runtime,
    [string]$Dist,
    [string]$Zadig = 'P:\Projekte\DAB\Qt-DAB-portable\zadig-2.9.exe'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$app = Join-Path $root 'apps\desktop'
if (-not $Dist) { $Dist = Join-Path $root 'dist\DAB-Classic-portable' }
$distParent = Split-Path -Parent $Dist

if (-not $SkipBuild) {
    Write-Host '== Kern (build-core.ps1)'
    & (Join-Path $PSScriptRoot 'build-core.ps1')
    Push-Location $app
    try {
        Write-Host '== Shell (pnpm install, pnpm tauri build --no-bundle)'
        pnpm install --frozen-lockfile
        if ($LASTEXITCODE -ne 0) { throw 'pnpm install fehlgeschlagen' }
        pnpm tauri build --no-bundle
        if ($LASTEXITCODE -ne 0) { throw 'tauri build fehlgeschlagen' }
    } finally { Pop-Location }
}

$exe = Join-Path $root 'target\release\dab-classic.exe'
if (-not (Test-Path $exe)) { throw "App-EXE fehlt: $exe (cargo tauri build)" }
$coreSrc = Join-Path $app 'src-tauri\resources\core'
if (-not (Test-Path (Join-Path $coreSrc 'dabcored.exe'))) { throw "Kern fehlt: $coreSrc\dabcored.exe (build-core.ps1)" }
$tiiSrc = Join-Path $app 'src-tauri\resources\tii\txdata.tii'
if (-not (Test-Path $tiiSrc)) { throw "TII-Datenbank fehlt: $tiiSrc" }

if (-not $WebView2Runtime) {
    $cab = Get-ChildItem (Join-Path $root 'third_party\webview2') -Filter 'Microsoft.WebView2.FixedVersionRuntime.*.x64.cab' -ErrorAction SilentlyContinue |
        Sort-Object { [version]($_.BaseName -replace '^Microsoft\.WebView2\.FixedVersionRuntime\.', '' -replace '\.x64$', '') } -Descending |
        Select-Object -First 1
    if ($cab) { $WebView2Runtime = $cab.FullName }
}

Write-Host "== Packen nach $Dist"
# Nutzdaten (settings.json, presets.json, Aufnahmen ...) eines frueheren
# Portable-Ordners ueberleben den Neuaufbau: vorher beiseitelegen, nach dem
# Packen (und nach dem ZIP, das leer bleiben soll) zurueckholen.
$dataKeep = $null
if (Test-Path "$Dist\data") {
    $hasUserData = Get-ChildItem "$Dist\data" -Recurse -File | Where-Object { $_.Name -ne 'README.txt' } | Select-Object -First 1
    if ($hasUserData) {
        $dataKeep = Join-Path $distParent '_data-keep'
        if (Test-Path $dataKeep) { Remove-Item -Recurse -Force $dataKeep }
        Move-Item "$Dist\data" $dataKeep
        Write-Host "== Nutzdaten beiseitegelegt: $dataKeep"
    }
}
if (Test-Path $Dist) { Remove-Item -Recurse -Force $Dist }
New-Item -ItemType Directory -Force $Dist, "$Dist\core", "$Dist\tii", "$Dist\data" | Out-Null

# Tauri (Windows) sucht Ressourcen relativ zur EXE: resource_core_path/resource_tii_path
# akzeptieren "core\" bzw. "tii\" direkt neben der EXE.
Copy-Item $exe (Join-Path $Dist 'dab-classic.exe')
# WebView2Loader.dll (webview2-com-sys legt sie neben die EXE; ohne sie startet
# die App mit 0xC0000135 "DLL nicht gefunden")
$loader = Join-Path $root 'target\release\WebView2Loader.dll'
if (-not (Test-Path $loader)) { throw "WebView2Loader.dll fehlt: $loader" }
Copy-Item $loader $Dist
Copy-Item "$coreSrc\*" "$Dist\core"
# FDK-AAC nur zur Laufzeit nachladbar; oeffentliches Release ohne die DLL (Entscheidung 20)
Remove-Item "$Dist\core\libfdk-aac-2.dll" -ErrorAction SilentlyContinue
Copy-Item $tiiSrc "$Dist\tii"
if (Test-Path $Zadig) { Copy-Item $Zadig $Dist }
$readme = Join-Path $root 'ANLEITUNG.txt'
if (Test-Path $readme) { Copy-Item $readme $Dist }
# Leere data\ mit Platzhalter, damit der Ordner auch im ZIP erhalten bleibt
Set-Content -Path "$Dist\data\README.txt" -Encoding utf8 -Value @'
Datenordner von DAB Classic (portabler Modus): settings.json, presets.json,
timers.json, recordings\, epg\, logos\, tii-files.csv. Liegt dieser Ordner
neben dab-classic.exe, schreibt die App nichts ins Benutzerprofil.
'@

$wvVersion = '(Evergreen des Systems)'
if ($WebView2Runtime) {
    if (-not (Test-Path $WebView2Runtime)) { throw "WebView2-Runtime nicht gefunden: $WebView2Runtime" }
    $wv = Join-Path $Dist 'webview2'
    New-Item -ItemType Directory -Force $wv | Out-Null
    Write-Host "== WebView2 entpacken: $WebView2Runtime"
    # .cab entpacken (expand.exe ist Teil von Windows); Microsoft packt in einen
    # Unterordner "Microsoft.WebView2.FixedVersionRuntime.<ver>.x64"
    & expand.exe -F:* $WebView2Runtime $wv | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "expand.exe fehlgeschlagen ($LASTEXITCODE)" }
    $inner = Get-ChildItem $wv -Directory | Select-Object -First 1
    if ($inner) {
        Get-ChildItem $inner.FullName -Force | Move-Item -Destination $wv -Force
        Remove-Item $inner.FullName -Recurse -Force
    }
    if (-not (Test-Path "$wv\msedgewebview2.exe")) { throw "webview2\msedgewebview2.exe fehlt nach dem Entpacken" }
    $wvVersion = (Get-Item "$wv\msedgewebview2.exe").VersionInfo.ProductVersion
}

$size = (Get-ChildItem $Dist -Recurse -File | Measure-Object Length -Sum).Sum / 1MB
Write-Host ("Portable-Ordner: {0} ({1:N0} MB, WebView2 {2})" -f $Dist, $size, $wvVersion)

if ($Zip) {
    $zipPath = Join-Path $distParent 'DAB-Classic-v3.0-dev-portable-win64.zip'
    if (Test-Path $zipPath) { Remove-Item $zipPath -Force }
    Write-Host "== ZIP: $zipPath"
    Compress-Archive -Path $Dist -DestinationPath $zipPath -CompressionLevel Optimal
    Write-Host ("ZIP: {0} ({1:N0} MB)" -f $zipPath, ((Get-Item $zipPath).Length / 1MB))
}

if ($dataKeep) {
    Remove-Item -Recurse -Force "$Dist\data"
    Move-Item $dataKeep "$Dist\data"
    Write-Host "== Nutzdaten zurueckgeholt nach $Dist\data"
}
