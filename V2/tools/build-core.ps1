# Baut den C++-Kern (libdabcore + dabcored) mit MSYS2 UCRT64 (GCC/CMake/Ninja)
# und legt dabcored.exe samt Laufzeit-DLLs nach apps/desktop/src-tauri/resources/core/.
#
#   .\tools\build-core.ps1            # Release
#   .\tools\build-core.ps1 -Debug
#   .\tools\build-core.ps1 -Clean
#   .\tools\build-core.ps1 -Test      # zusaetzlich ctest
param(
    [switch]$Debug,
    [switch]$Clean,
    [switch]$Test,
    [string]$Msys = 'C:\msys64'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$src = Join-Path $root 'core-cpp'
$build = Join-Path $src 'build'
$dest = Join-Path $root 'apps\desktop\src-tauri\resources\core'
$ucrt = Join-Path $Msys 'ucrt64\bin'
$cfg = if ($Debug) { 'Debug' } else { 'Release' }

if (-not (Test-Path (Join-Path $ucrt 'g++.exe'))) { throw "MSYS2 UCRT64 nicht gefunden unter $ucrt" }
if ($Clean -and (Test-Path $build)) { Remove-Item -Recurse -Force $build }

$env:PATH = "$ucrt;" + $env:PATH
$env:MSYSTEM = 'UCRT64'
$env:PKG_CONFIG_PATH = Join-Path $Msys 'ucrt64\lib\pkgconfig'

Write-Host "== CMake ($cfg) -> $build"
& (Join-Path $ucrt 'cmake.exe') -S $src -B $build -G Ninja "-DCMAKE_BUILD_TYPE=$cfg" `
    "-DCMAKE_C_COMPILER=$ucrt\gcc.exe" "-DCMAKE_CXX_COMPILER=$ucrt\g++.exe"
if ($LASTEXITCODE -ne 0) { throw 'CMake-Konfiguration fehlgeschlagen' }

Write-Host '== Ninja'
& (Join-Path $ucrt 'ninja.exe') -C $build
if ($LASTEXITCODE -ne 0) { throw 'Build fehlgeschlagen' }

if ($Test) {
    Write-Host '== ctest'
    & (Join-Path $ucrt 'ctest.exe') --test-dir $build --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'Tests fehlgeschlagen' }
}

# Laufzeit-DLLs anhand der Importtabelle einsammeln (rekursiv, nur aus ucrt64\bin).
Write-Host "== Kopiere nach $dest"
New-Item -ItemType Directory -Force $dest | Out-Null
$exe = Join-Path $build 'dabcored.exe'
Copy-Item $exe $dest -Force
$objdump = Join-Path $ucrt 'objdump.exe'
$seen = @{}
$queue = New-Object System.Collections.Queue
$queue.Enqueue($exe)
while ($queue.Count -gt 0) {
    $f = $queue.Dequeue()
    $deps = & $objdump -p $f 2>$null | Select-String 'DLL Name: (.+)' | ForEach-Object { $_.Matches[0].Groups[1].Value.Trim() }
    foreach ($d in $deps) {
        if ($seen.ContainsKey($d)) { continue }
        $seen[$d] = $true
        $p = Join-Path $ucrt $d
        if (Test-Path $p) {
            Copy-Item $p $dest -Force
            $queue.Enqueue($p)
        }
    }
}
# Zur Laufzeit nachgeladene Bibliotheken (nicht in der Importtabelle):
foreach ($opt in 'libhackrf.dll', 'libusb-1.0.dll', 'librtlsdr.dll', 'libfdk-aac-2.dll') {
    $p = Join-Path $ucrt $opt
    if (Test-Path $p) { Copy-Item $p $dest -Force }
}
Get-ChildItem $dest | ForEach-Object { Write-Host ("   {0,-28} {1,8:N0} kB" -f $_.Name, ($_.Length / 1kb)) }
Write-Host "Fertig: $(Join-Path $dest 'dabcored.exe')"
