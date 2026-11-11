# gen-desktop-icons.ps1 -- generate Tauri icon set from a procedurally-
# drawn PNG. Run once; commits the output so subsequent builds skip
# this step.
#
# Why generate vs. ship a static logo? We don't have a designed brand
# mark in the repo yet. This placeholder unblocks Windows MSI / macOS
# DMG / Linux deb builds. When marketing hands us a real 1024x1024
# logo, drop it as source.png and re-run.
#
# Note on the original (broken) System.Drawing approach: PowerShell
# 5.1's New-Object constructor overload resolution misbehaved on
# Drawing.Bitmap / GraphicsPath calls (Object[] -> UInt32 conversion
# error). We sidestep by generating the PNG via a tiny Go program
# (scripts/gen-source-icon) — Go's stdlib image/png is portable and
# we already have a Go toolchain.
#
# Usage:
#   scripts\gen-desktop-icons.ps1                 # source + all variants
#   scripts\gen-desktop-icons.ps1 -SourcePath x.png   # use existing PNG
#
# Requirements:
#   - Go 1.25+ on PATH (only for the placeholder source step; skipped
#     if -SourcePath points at an existing file)
#   - cargo + tauri-cli on PATH (cargo install tauri-cli)

[CmdletBinding()]
param(
    [string]$SourcePath = ""
)

$ErrorActionPreference = "Stop"
$Root = (Resolve-Path "$PSScriptRoot\..").Path
$IconDir = Join-Path $Root "apps\client-desktop\src-tauri\icons"
New-Item -ItemType Directory -Force -Path $IconDir | Out-Null

if (-not $SourcePath) {
    $SourcePath = Join-Path $IconDir "source.png"
}

if (-not (Test-Path $SourcePath)) {
    Write-Host "==> Generating 1024x1024 placeholder PNG via Go"
    Push-Location (Join-Path $Root "scripts\gen-source-icon")
    try {
        go run . -out $SourcePath
        if ($LASTEXITCODE -ne 0) {
            throw "gen-source-icon exited $LASTEXITCODE"
        }
    } finally {
        Pop-Location
    }
} else {
    Write-Host "==> Using existing source: $SourcePath"
}

# Tauri's icon command emits the full set (32x32.png, 128x128.png,
# 128x128@2x.png, icon.ico, icon.icns, plus macOS Square logo
# variants) under <src-tauri>/icons/.
Write-Host "==> cargo tauri icon $SourcePath"
Push-Location (Join-Path $Root "apps\client-desktop\src-tauri")
try {
    cargo tauri icon $SourcePath
    if ($LASTEXITCODE -ne 0) {
        throw "cargo tauri icon exited $LASTEXITCODE"
    }
} finally {
    Pop-Location
}

Write-Host ""
Write-Host "==> Generated icons:"
Get-ChildItem $IconDir -File `
    | Where-Object { $_.Extension -in ".png", ".ico", ".icns" } `
    | Select-Object Name, @{Name="KB";Expression={[math]::Round($_.Length/1KB, 1)}}
