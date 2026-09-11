# Erzeugt den portablen Ordner fuer DAB Classic (Entscheidung 12):
#   DAB Classic.exe + core\ (dabcored.exe + DLLs) + data\ + WebView2 Fixed-Version-Runtime
#   + zadig + ANLEITUNG. Kein Installer, keine Start.bat.
#
#   .\tools\deploy-portable.ps1                 # baut Kern + App (release), packt nach dist-portable\
#   .\tools\deploy-portable.ps1 -SkipBuild
#   .\tools\deploy-portable.ps1 -WebView2Runtime C:\Downloads\Microsoft.WebView2.FixedVersionRuntime.x64.cab
#
# Die Fixed-Version-Runtime (~180 MB) wird von Microsoft als .cab verteilt
# (https://developer.microsoft.com/microsoft-edge/webview2/#download). Ohne
# Angabe wird sie nicht beigelegt; die App nutzt dann die installierte
# Evergreen-Runtime. Tauri/wry respektieren WEBVIEW2_BROWSER_EXECUTABLE_FOLDER;
# die App setzt diese Variable selbst, wenn neben der EXE ein Ordner
# "webview2\" liegt (siehe apps/desktop/src-tauri/src/main.rs).
param(
    [switch]$SkipBuild,
    [string]$WebView2Runtime,
    [string]$Zadig = 'P:\Projekte\DAB\Qt-DAB-portable\zadig-2.9.exe'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$app = Join-Path $root 'apps\desktop'
$dist = Join-Path $root 'dist-portable'

if (-not $SkipBuild) {
    & (Join-Path $PSScriptRoot 'build-core.ps1')
    Push-Location $app
    try {
        pnpm install --frozen-lockfile
        pnpm tauri build --no-bundle
        if ($LASTEXITCODE -ne 0) { throw 'tauri build fehlgeschlagen' }
    } finally { Pop-Location }
}

$exe = Join-Path $root 'target\release\dab-classic.exe'
if (-not (Test-Path $exe)) { throw "App-EXE fehlt: $exe" }

if (Test-Path $dist) { Remove-Item -Recurse -Force $dist }
New-Item -ItemType Directory -Force $dist, "$dist\core", "$dist\data" | Out-Null

Copy-Item $exe (Join-Path $dist 'DAB Classic.exe')
Copy-Item (Join-Path $app 'src-tauri\resources\core\*') "$dist\core"
if (Test-Path $Zadig) { Copy-Item $Zadig $dist }
$readme = Join-Path $root 'docs\ANLEITUNG.txt'
if (Test-Path $readme) { Copy-Item $readme $dist }

if ($WebView2Runtime) {
    if (-not (Test-Path $WebView2Runtime)) { throw "WebView2-Runtime nicht gefunden: $WebView2Runtime" }
    $wv = Join-Path $dist 'webview2'
    New-Item -ItemType Directory -Force $wv | Out-Null
    # .cab entpacken (expand.exe ist Teil von Windows)
    & expand.exe -F:* $WebView2Runtime $wv | Out-Null
    # Microsoft packt in einen Unterordner "Microsoft.WebView2.FixedVersionRuntime.<ver>.x64"
    $inner = Get-ChildItem $wv -Directory | Select-Object -First 1
    if ($inner) {
        Get-ChildItem $inner.FullName | Move-Item -Destination $wv -Force
        Remove-Item $inner.FullName -Recurse -Force
    }
}

$size = (Get-ChildItem $dist -Recurse | Measure-Object Length -Sum).Sum / 1MB
Write-Host ("Portable-Ordner: {0} ({1:N0} MB)" -f $dist, $size)
