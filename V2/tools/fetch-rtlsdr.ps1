# Holt librtlsdr.dll aus dem Osmocom-Windows-Release (MSYS2 hat kein
# rtl-sdr-Paket) nach core-cpp\third_party\bin\; build-core.ps1 kopiert sie
# von dort in die Tauri-Ressourcen. Die DLL braucht nur libusb-1.0.dll, die
# schon aus ucrt64 mitkommt. *.dll ist im Repository ignoriert – nach einem
# frischen Checkout einmal ausfuehren.
#
#   .\tools\fetch-rtlsdr.ps1                 # neueste 64-Bit-Version
#   .\tools\fetch-rtlsdr.ps1 -Version 20260906
param([string]$Version = '')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$dest = Join-Path $root 'core-cpp\third_party\bin'
$base = 'https://ftp.osmocom.org/binaries/windows/rtl-sdr/'
$ua = 'Mozilla/5.0'

if (-not $Version) {
    $index = Invoke-WebRequest -Uri $base -UseBasicParsing -TimeoutSec 30 -UserAgent $ua
    $names = $index.Links | ForEach-Object { $_.href } | Where-Object { $_ -match '^rtl-sdr-64bit-(\d+)\.zip$' }
    if (-not $names) { throw "kein rtl-sdr-64bit-*.zip unter $base gefunden" }
    $Version = ($names | ForEach-Object { [regex]::Match($_, '(\d+)').Groups[1].Value } | Sort-Object | Select-Object -Last 1)
}
$zipName = "rtl-sdr-64bit-$Version.zip"
$tmp = Join-Path ([System.IO.Path]::GetTempPath()) "rtlsdr-$Version"
New-Item -ItemType Directory -Force $tmp | Out-Null
$zip = Join-Path $tmp $zipName
Write-Host "== Lade $base$zipName"
Invoke-WebRequest -Uri ($base + $zipName) -OutFile $zip -UseBasicParsing -TimeoutSec 120 -UserAgent $ua
Expand-Archive -Path $zip -DestinationPath $tmp -Force
$dll = Get-ChildItem -Recurse $tmp -Filter 'librtlsdr.dll' | Select-Object -First 1
if (-not $dll) { throw 'librtlsdr.dll nicht im Archiv' }
New-Item -ItemType Directory -Force $dest | Out-Null
Copy-Item $dll.FullName $dest -Force
Write-Host ("Fertig: {0} ({1:N0} Byte, Release {2})" -f (Join-Path $dest 'librtlsdr.dll'), $dll.Length, $Version)
