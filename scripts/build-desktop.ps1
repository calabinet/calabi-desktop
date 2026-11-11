# build-desktop.ps1 -- Windows release pipeline for the Calabi desktop shell.
#
# Mirrors scripts/build-desktop.sh: build the SPA, build the daemon
# binary, copy it into apps/client-desktop/, then `cargo tauri build`
# which spits out the .msi + NSIS .exe under
# apps/client-desktop/src-tauri/target/release/bundle/.
#
# SIGNING: the installers are signed ONLY when $env:CALABI_SIGNING_THUMBPRINT is
# set (see "Authenticode signing" below). Without it you get UNSIGNED installers
# and every Windows user hits a SmartScreen "Publisher: Unknown" block on first
# run — the script warns loudly rather than failing, so a dev build still works.
# (This comment block used to claim the output was signed and to document that
# env var while nothing ever read it. It's wired now.)
#
# Prereqs:
#   - rustup-installed cargo (1.75+)
#   - tauri-cli installed: `cargo install tauri-cli`
#   - Node.js 18+
#   - Windows SDK + MSVC build tools (Build Tools for Visual Studio)
#   - WebView2 runtime (preinstalled on Windows 11; install link in docs)
#   - For SIGNED builds: a code-signing cert in the local cert store +
#     $env:CALABI_SIGNING_THUMBPRINT=<its sha1 thumbprint>. Tauri drives
#     signtool itself (it ships with the Windows SDK). Optional:
#     $env:CALABI_SIGNING_TIMESTAMP_URL (defaults to DigiCert's RFC-3161 URL).
#     NOTE: signing does NOT silence SmartScreen on day one, and EV buys nothing
#     here -- Microsoft removed EV's automatic reputation in 2024; OV and EV are
#     now one row in their own table ("Warning until reputation accumulates").
#     Signing is still worth it: reputation accrues on the CERT and carries to
#     later versions, whereas unsigned files restart from zero every release.
# #5.2.
#
# Usage from repo root:
#   pwsh scripts\build-desktop.ps1
#   pwsh scripts\build-desktop.ps1 -SkipSpa
#   $env:CALABI_SIGNING_THUMBPRINT='ab12…'; pwsh scripts\build-desktop.ps1 -EdgeCa …

[CmdletBinding()]
param(
    [switch]$SkipSpa,
    [string]$Target = "",
    # Release bake (mirrors scripts/package-release-client.sh). When -EdgeCa is
    # given (or $env:EDGE_CA is set) the bundled daemon is built as a RELEASE
    # binary: real version, production bff-console, and the given edge-CA ROOT
    # baked in. Without it you get a DEV daemon (version=dev, bff=localhost, dev
    # CA) — fine for local testing, NEVER for production.
    [string]$EdgeCa = $env:EDGE_CA,
    [string]$BffConsole = $(if ($env:BFF_CONSOLE) { $env:BFF_CONSOLE } else { "https://api.calabi.net" }),
    # Browser-facing console origin the client's login page links to for
    # registration. Separate from -BffConsole on purpose: that's the API host
    # (api.calabi.net), this is the SPA host (console.calabi.net). Baked in;
    # $CALABI_CONSOLE_WEB still overrides at runtime.
    [string]$ConsoleWeb = $(if ($env:CONSOLE_WEB) { $env:CONSOLE_WEB } else { "https://console.calabi.net" }),
    # Kept for compatibility with existing callers; desktop builds now always
    # compile their selected core with independent product metadata.
    [switch]$ForceBuildDaemon,
    # Baked default edge. EMPTY like the release packager: no default edge,
    # discovered via bff-console. Present so this build path bakes the same
    # set of -X as package-release-client.sh (checked by
    # scripts/lint/check-release-ldflags.mjs).
    [string]$EdgeDefault = $env:EDGE_DEFAULT
)

$ErrorActionPreference = "Stop"
# Re-enter once through the shared cache lease; nested build scripts reuse it.
. (Join-Path $PSScriptRoot 'dev/build-cache.ps1')
if (-not (Test-CalabiBuildLease)) {
    & (Join-Path $PSScriptRoot 'dev/build.ps1') __script $PSCommandPath $PSBoundParameters
    exit $LASTEXITCODE
}
$Root = (Resolve-Path "$PSScriptRoot\..").Path
Set-Location $Root

if ($EdgeCa -and ($Target -or $env:CARGO_BUILD_TARGET -or $env:PROCESSOR_ARCHITECTURE -ne 'AMD64')) {
    throw 'The official Windows installer requires a native x64 build with no Cargo target override.'
}

$sourceRecord = Join-Path $Root 'product-build.json'
if (Test-Path $sourceRecord) {
    $sourceRecordData = Get-Content $sourceRecord -Raw | ConvertFrom-Json
    if (-not $sourceRecordData.local_override) { $SkipSpa = $true; Write-Host 'Using the pinned core console bundle' }
}
Write-Host "==> 1/4 SPA build"
if (-not $SkipSpa) {
    Push-Location apps\client\internal\status\ui
    npm ci --no-audit --no-fund
    npm run build
    Pop-Location
} else {
    Write-Host "    (skipped via -SkipSpa)"
}

Write-Host "==> 2/4 calabi daemon build"
# Single source of truth: bin\calabi.exe at repo root, matching the
# Makefile's BIN_DIR convention. Every doc, every runbook, every
# manual `go build` from the user lands here. Previously we'd write
# to apps\client-desktop\calabi.exe AND apps\client\bin\calabi.exe
# AND src-tauri\target\release\calabi.exe in different scripts and
# devs kept running stale versions -- now there's one canonical path.
if (-not (Test-Path "bin")) {
    New-Item -ItemType Directory -Force -Path "bin" | Out-Null
}

# The bundled daemon is baked into calabi-desktop.exe (build.rs include_bytes),
# so it MUST be a proper release build for production — otherwise the shipped
# desktop app runs a daemon that points at localhost (defaultBFFConsole) with the
# committed DEV edge CA. Mirror scripts/package-release-client.sh's bake when an
# edge CA is provided; otherwise warn loudly that this is a dev daemon.
$Version = (Get-Content apps/client-desktop/VERSION -Raw).Trim()
$CoreVersion = (Get-Content VERSION -Raw).Trim()
$env:CALABI_BUNDLED_CORE_VERSION = $CoreVersion
$env:CALABI_BUNDLED_CORE_COMMIT = (& python (Join-Path $Root 'scripts/products/product.py') core-commit desktop).Trim()
if ($LASTEXITCODE -ne 0) { throw 'Cannot resolve actual bundled core source' }

# Keep the bundle version aligned with apps/client-desktop/VERSION. The installer (.msi/.nsis) version comes from
# tauri.conf.json — NOT the baked daemon's -ldflags version — so without this
# the bundle ships a stale version even though the daemon inside is $Version
# (mismatched installer, and a red-line collision if it reuses a published
# version). Cargo.toml's package version is kept aligned too. Idempotent; the
# edits are real (committed with the release), not reverted like the edge-CA.
# Use ReadAllText/WriteAllText with UTF-8 (no BOM) so PS 5.1 doesn't inject a
# BOM / rewrite line endings into the tracked JSON/TOML.
if ($Version -and $Version -ne "dev") {
    $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
    $tauriConf = (Resolve-Path "apps\client-desktop\src-tauri\tauri.conf.json").Path
    $cargoToml = (Resolve-Path "apps\client-desktop\src-tauri\Cargo.toml").Path
    # tauri.conf.json has exactly one top-level "version" key.
    $conf = [System.IO.File]::ReadAllText($tauriConf)
    $conf = $conf -replace '("version"\s*:\s*")[^"]*(")', "`${1}$Version`${2}"
    [System.IO.File]::WriteAllText($tauriConf, $conf, $utf8NoBom)
    # Cargo.toml: only the [package] version sits at column 0; dep version specs
    # are indented / inline-table, so the multiline ^anchor is safe.
    $toml = [System.IO.File]::ReadAllText($cargoToml)
    $toml = $toml -replace '(?m)^version = "[^"]*"', "version = `"$Version`""
    [System.IO.File]::WriteAllText($cargoToml, $toml, $utf8NoBom)
    Write-Host "    synced desktop bundle version -> $Version (tauri.conf.json + Cargo.toml)"
}
$embedCa = "apps\client\internal\transport\certs\edge-ca.pem"
$release = [bool]$EdgeCa

if ($release) {
    if (-not (Test-Path $EdgeCa)) { throw "EDGE_CA not found: $EdgeCa" }
    $caText = Get-Content $EdgeCa -Raw
    if ($caText -notmatch "BEGIN CERTIFICATE") { throw "EDGE_CA is not a PEM certificate: $EdgeCa" }
    if ($caText -match "PRIVATE KEY") { throw "refusing: EDGE_CA contains a PRIVATE KEY — pass the public ca.crt only, never ca.key" }
    if ((Test-Path $embedCa) -and ((Get-Content $embedCa -Raw) -eq $caText)) {
        throw "refusing: EDGE_CA is byte-identical to the committed DEV CA ($embedCa). Pass your deployment's data/edge-certs/ca.crt instead."
    }
    switch -Regex ($BffConsole) {
        '^https://' { }
        '^http://(127\.0\.0\.1|localhost)' { Write-Warning "BFF_CONSOLE is a LOCAL/dev address ($BffConsole) — release clients won't reach a real server." }
        '^http://' { Write-Warning "BFF_CONSOLE is plain HTTP ($BffConsole) — prefer https://." }
        default { throw "BFF_CONSOLE must be a URL (got: $BffConsole)" }
    }
    Write-Host "    RELEASE daemon: core=$CoreVersion desktop=$Version  bff=$BffConsole  console=$ConsoleWeb"
    Write-Host "    embedding edge CA: $EdgeCa"
    $caBeforeBuild = [IO.File]::ReadAllBytes((Resolve-Path $embedCa))
    Copy-Item -Force $EdgeCa $embedCa
} else {
    Write-Warning "no -EdgeCa / `$env:EDGE_CA — building a DEV daemon (version=dev, bff=localhost, DEV edge CA). NOT for production. For a release: `$env:EDGE_CA='deploy\compose\data\edge-certs\ca.crt'; scripts\build-desktop.ps1"
}

# Desktop installers compile their pinned core with independent product metadata.
# A plain core archive has no desktop product version and cannot be reused here.
$stagedFromRelease = $false
try {
    # A desktop daemon carries its product version as well as its core version.
    # Core release archives intentionally do not carry that desktop build metadata.
    if (-not $stagedFromRelease) {
        Push-Location apps\client
        if ($release) {
            $env:CGO_ENABLED = "0"
            go build -trimpath -buildvcs=false "-ldflags=-s -w -X main.version=$CoreVersion -X main.desktopProductVersion=$Version -X main.defaultBFFConsole=$BffConsole -X main.defaultConsoleWeb=$ConsoleWeb -X main.defaultServer=$EdgeDefault" -o ..\..\bin\calabi.exe .\cmd\calabi
        } else {
            go build -o ..\..\bin\calabi.exe .\cmd\calabi
        }
        # Native exit codes don't trip $ErrorActionPreference='Stop'. Without this
        # a failed compile leaves the PREVIOUS bin\calabi.exe in place and the
        # script happily bakes that stale daemon into the installer.
        if ($LASTEXITCODE -ne 0) { throw "go build FAILED (exit $LASTEXITCODE) -- bin\calabi.exe would be stale" }
        Pop-Location
    }
} finally {
    # Always restore the committed dev CA placeholder so the working tree is
    # never left holding a production cert (mirrors the .sh trap).
    if ($release) { [IO.File]::WriteAllBytes((Join-Path $Root $embedCa), $caBeforeBuild) }
}
Write-Host "    wrote bin\calabi.exe (canonical)"

# Copy into the Cargo release dir so the supervisor's sibling-binary
# fallback (resolve_binary in daemon.rs) finds it when the user runs
# `cargo tauri build` next. This copy is build-script-managed -- do
# NOT edit it by hand, it gets clobbered every build.
$tauriBinDir = "apps\client-desktop\src-tauri\target\release"
if (-not (Test-Path $tauriBinDir)) {
    New-Item -ItemType Directory -Force -Path $tauriBinDir | Out-Null
}
Copy-Item -Force "bin\calabi.exe" "$tauriBinDir\calabi.exe"
Write-Host "    copied to $tauriBinDir\calabi.exe (tauri sibling fallback)"

# Stage the daemon where build.rs bakes it INTO the exe (include_bytes!):
# at compile time build.rs copies binaries\calabi.exe into the binary, and at
# runtime the supervisor extracts it (see src-tauri/src/daemon.rs). This is what
# makes calabi-desktop.exe self-contained — the exe itself carries the daemon,
# no sidecar file. Must be staged BEFORE cargo tauri build so build.rs sees it.
$embedDir = "apps\client-desktop\src-tauri\binaries"
if (-not (Test-Path $embedDir)) {
    New-Item -ItemType Directory -Force -Path $embedDir | Out-Null
}
Copy-Item -Force "bin\calabi.exe" "$embedDir\calabi.exe"
Write-Host "    staged to $embedDir\calabi.exe (baked into calabi-desktop.exe)"

# Tauri-build refuses to compile without icons/icon.ico (Win RC step).
# Auto-generate the placeholder set the first time. Subsequent builds
# skip this -- once gen-desktop-icons.ps1 wrote icon.ico we never
# regenerate.
#
# Invoked via the & call operator so it works under both Windows
# PowerShell 5.1 (powershell.exe) and PowerShell 7+ (pwsh.exe). The
# previous version assumed pwsh was on PATH.
if (-not (Test-Path "apps\client-desktop\src-tauri\icons\icon.ico")) {
    Write-Host "==> Icons missing -- running gen-desktop-icons.ps1"
    & (Join-Path $PSScriptRoot "gen-desktop-icons.ps1")
    if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne $null) {
        throw "gen-desktop-icons.ps1 exited $LASTEXITCODE"
    }
}

# ---------------------------------------------------------------------------
# Authenticode signing (optional, driven by $env:CALABI_SIGNING_THUMBPRINT).
#
# Tauri signs the app exe AND both installers when bundle.windows.
# certificateThumbprint is set, invoking signtool from the Windows SDK itself —
# so we inject the thumbprint into tauri.conf.json for the duration of the build
# and ALWAYS put the file back (same swap/restore discipline as the edge CA).
# A thumbprint is machine- and cert-specific: it must never land in the tracked
# config.
#
# Restore writes back the exact pre-injection TEXT rather than `git checkout` —
# the version sync earlier in this script edits the same file and those edits are
# meant to survive (they get committed with the release); checking out would
# revert the version bump too.
# ---------------------------------------------------------------------------
$signConf   = (Resolve-Path "apps\client-desktop\src-tauri\tauri.conf.json").Path
$signUtf8   = New-Object System.Text.UTF8Encoding($false)
$confBeforeSign = $null
$thumb = $env:CALABI_SIGNING_THUMBPRINT
if ($thumb) {
    # certmgr / the cert dialog copy thumbprints with spaces (and sometimes a
    # leading U+200E); signtool wants bare hex.
    $thumb = ($thumb -replace '[^0-9A-Fa-f]', '')
    $tsUrl = if ($env:CALABI_SIGNING_TIMESTAMP_URL) { $env:CALABI_SIGNING_TIMESTAMP_URL } else { 'http://timestamp.digicert.com' }
    $confBeforeSign = [System.IO.File]::ReadAllText($signConf)
    $j = $confBeforeSign | ConvertFrom-Json
    if (-not $j.bundle.PSObject.Properties['windows']) {
        $j.bundle | Add-Member -NotePropertyName windows -NotePropertyValue ([pscustomobject]@{}) -Force
    }
    $j.bundle.windows | Add-Member -NotePropertyName certificateThumbprint -NotePropertyValue $thumb   -Force
    $j.bundle.windows | Add-Member -NotePropertyName digestAlgorithm       -NotePropertyValue 'sha256' -Force
    $j.bundle.windows | Add-Member -NotePropertyName timestampUrl          -NotePropertyValue $tsUrl   -Force
    # Depth: the config nests several levels; PS 5.1 defaults to 2 and would
    # silently stringify the rest into "System.Object[]".
    [System.IO.File]::WriteAllText($signConf, ($j | ConvertTo-Json -Depth 100), $signUtf8)
    $shown = if ($thumb.Length -gt 8) { $thumb.Substring(0, 8) } else { $thumb }
    Write-Host "    SIGNING enabled: thumbprint $shown... (timestamp: $tsUrl)"
} else {
    # ASCII only: PS 5.1 reads a UTF-8-no-BOM .ps1 as ANSI, so any non-ASCII in a
    # string literal prints as mojibake (several older warnings in this file do).
    Write-Warning ("no `$env:CALABI_SIGNING_THUMBPRINT -- installers will be UNSIGNED. " +
        "Windows SmartScreen shows 'Publisher: Unknown' and blocks first run for every user. " +
        "Fine for a dev build; for a real release set the thumbprint. " +
        "See the release verification instructions.")
}

Write-Host "==> 3/4 cargo tauri build"
Push-Location apps\client-desktop\src-tauri
$rdpStage = $null
try {
    $bundleArgs = @('build', '--bundles', 'nsis')
    if ($EdgeCa) {
        $tempRoot = (Resolve-Path binaries).Path
        $rdpStage = (New-Item -ItemType Directory -Path (Join-Path $tempRoot "calabi-rdp-release-$([guid]::NewGuid())")).FullName
        $rdpRuntime = Join-Path $rdpStage 'rdp'
        & python "$Root/tools/rdp-p0/build-windows-helper.py" $rdpRuntime
        if ($LASTEXITCODE -ne 0) { throw 'Windows RDP helper build or self-test failed.' }
        $config = Join-Path $rdpStage 'tauri-rdp.json'
        $resource = "binaries/$(Split-Path -Leaf $rdpStage)/rdp"
        $overlay = @{bundle=@{resources=@{$resource='rdp/'}}}
        [IO.File]::WriteAllText($config, ($overlay | ConvertTo-Json -Depth 4), $signUtf8)
        $bundleArgs += @('--config', $config)
    }
    # Windows ships ONLY the NSIS installer — it registers the daemon as a
    # LocalSystem service via its install hook (nsis-hooks.nsh). The WiX MSI has
    # no such hook (app-only), so we don't build it. --bundles is explicit rather
    # than relying on tauri.windows.conf.json target-merge semantics.
    if ($Target) { $bundleArgs += @('--target', $Target) }
    & cargo tauri @bundleArgs
    # $ErrorActionPreference='Stop' does NOT catch a native exe's non-zero exit,
    # and nothing here checked $LASTEXITCODE. A failed bundle therefore fell
    # straight through to "4/4 bundle output", which staged the PREVIOUS build's
    # installers into dist/release and printed "Done" -- i.e. a signing failure
    # (or any bundle error) silently published stale artifacts as if fresh.
    if ($LASTEXITCODE -ne 0) { throw "cargo tauri build FAILED (exit $LASTEXITCODE) -- refusing to stage stale installers" }
} finally {
    Pop-Location
    if ($confBeforeSign) {
        [System.IO.File]::WriteAllText($signConf, $confBeforeSign, $signUtf8)
        Write-Host "    restored tauri.conf.json (thumbprint kept out of the tracked file)"
    }
    if ($rdpStage -and (Split-Path -Parent $rdpStage) -eq $tempRoot) {
        Remove-Item -LiteralPath $rdpStage -Recurse -Force
    }
}

Write-Host "==> 4/4 bundle output"
$targetRoot = if ($env:CARGO_TARGET_DIR) { $env:CARGO_TARGET_DIR } else { "apps\client-desktop\src-tauri\target" }
$bundleDir = Join-Path $targetRoot "release/bundle"
if (Test-Path $bundleDir) {
    Get-ChildItem $bundleDir -Recurse -File `
        | Where-Object { $_.Extension -in ".msi", ".exe" } `
        | Select-Object FullName, Length
    Write-Host ""
    Write-Host "Done. Installers under $bundleDir\."
} else {
    Write-Host "    (no bundle directory -- cargo tauri build may have failed)"
}

# Stage the x64 installers into dist\release-desktop\ under the STABLE download-page names
# (web/www Download.tsx) so they sit alongside the client archives, ready to
# upload to OSS / download.calabi.net/latest/ and verify against SHA256SUMS.
# Only for a plain x64 build (no cross --Target); arm64/other targets are skipped.
if (-not $Target) {
    $dist = "dist\release-desktop"
    New-Item -ItemType Directory -Force -Path $dist | Out-Null
    $stage = @{
        # NSIS setup.exe only (service-installing installer); MSI is no longer built.
        "$bundleDir\nsis\Calabi_${Version}_x64-setup.exe" = "calabi-desktop-windows-x64-setup.exe"
    }
    # Absolute — [System.IO.File] resolves relative paths against the .NET process
    # CWD, which does NOT follow PowerShell's Set-Location (same reason the
    # version-sync above uses Resolve-Path).
    $sums = Join-Path (Resolve-Path $dist).Path "SHA256SUMS"
    foreach ($src in $stage.Keys) {
        if (-not (Test-Path $src)) { throw "Expected installer not found: $src" }
        $name = $stage[$src]
        Copy-Item -Force $src (Join-Path $dist $name)
        Write-Host "    staged -> $dist\$name"
        # keep SHA256SUMS in sync (drop any stale line for this name, then append).
        #
        # Write it with WriteAllText and explicit LF. Set-Content/Add-Content emit
        # CRLF on Windows, which rewrites EVERY line of the list (including the
        # seven the bash packager wrote with LF) and leaves a \r glued to each
        # FILENAME — `sha256sum -c SHA256SUMS` then fails every entry with
        # "No such file or directory", so a user verifying the download concludes
        # the artifacts are corrupt. Same class of PS 5.1 mangling the version-sync
        # above already dodges for tauri.conf.json / Cargo.toml.
        if ($src) {
            $h = (Get-FileHash (Join-Path $dist $name) -Algorithm SHA256).Hash.ToLower()
            $lines = @()
            if (Test-Path $sums) { $lines = @(Get-Content $sums | Where-Object { $_.Trim() -and $_ -notmatch [regex]::Escape($name) }) }
            $lines += "$h  $name"
            [System.IO.File]::WriteAllText($sums, (($lines -join "`n") + "`n"), (New-Object System.Text.ASCIIEncoding))
        }
    }

    # Optional: sign the NSIS installer into a self-update manifest entry.
    # Skipped unless BOTH the signing key and the public download base are set;
    # a dev build just produces the installer.
    if ($env:CALABI_UPDATE_PRIVKEY -and $env:CALABI_DOWNLOAD_BASE) {
        $nsisStaged = Join-Path $dist "calabi-desktop-windows-x64-setup.exe"
        if (Test-Path $nsisStaged) {
            $manifest = Join-Path $dist "latest.json"
            $url = ($env:CALABI_DOWNLOAD_BASE.TrimEnd('/')) + "/calabi-desktop-windows-x64-setup.exe"
            Write-Host "==> updatekit merge (windows-x86_64 -> $manifest)"
            go run ./scripts/updatekit merge --manifest $manifest --version $Version --platform windows-x86_64 --installer $nsisStaged --url $url --privkey $env:CALABI_UPDATE_PRIVKEY
            if ($LASTEXITCODE -ne 0) { throw "updatekit merge FAILED (exit $LASTEXITCODE)" }
        }
    }
}
