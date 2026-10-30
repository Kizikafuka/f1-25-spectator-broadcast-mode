# Builds the addon and packs it. Needs zig on PATH and the Swofty SDK headers in native\include.
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot
$m = Get-Content swofty.json -Raw | ConvertFrom-Json
if (-not (Test-Path "native\include\swofty\mixin.hpp")) { throw "Copy the Swofty native SDK headers into native\include first (see native\include\README.txt)." }
$info = [Text.Encoding]::UTF8.GetByteCount("by $($m.author), v$($m.version)`n$($m.description)")
if ($info -gt 250) { throw "description too long for the game ($info of 250 bytes)" }
zig c++ -target x86_64-windows-gnu -std=c++23 -O2 -Wall -Wextra -shared -s "-DADDON_VERSION_RAW=$($m.version)" -I native\include `
    -o native\kizi_spectator_hud.dll native\src\spectator_hud.cpp res\dll.rc -lkernel32 -lws2_32
if ($LASTEXITCODE -ne 0) { throw "compile failed" }
$stage = Join-Path $env:TEMP "spectator_broadcast_mode_stage"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force "$stage\native" | Out-Null
Copy-Item swofty.json, icon.png, README.md, LICENSE $stage
Copy-Item native\kizi_spectator_hud.dll "$stage\native"
Copy-Item erp, ui -Destination $stage -Recurse
New-Item -ItemType Directory -Force dist | Out-Null
$pkg = "dist\$($m.id)-$($m.version).swoftyaddon"
if (Test-Path $pkg) { Remove-Item $pkg }
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [IO.Compression.ZipFile]::Open((Join-Path $PWD $pkg), "Create")
try {
    Get-ChildItem -Recurse -File $stage | ForEach-Object {
        $rel = $_.FullName.Substring($stage.Length + 1).Replace('\', '/')
        [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $_.FullName, $rel) | Out-Null
    }
} finally { $zip.Dispose() }
Write-Host "packed $pkg"
