# Changelog

## 3.0.0 — 2026-11-11

## Changes

- **Changed:** Calabi Desktop now has its own public repository, product version, installer checksums and release provenance.
- **Added:** The native overview displays the desktop product version. Settings show the desktop version, running core version, bundled core version and pinned core commit separately.
- **Changed:** Native desktop update checks use the desktop package channel. Opening the daemon console in a browser shows the running core version and uses the core update interface.
- **Fixed:** macOS builds can reuse verified local OpenSSL and FreeRDP source archives across builds. Offline mode stops on a missing or mismatched archive instead of downloading it again.

## Install or upgrade

Use calabi-desktop-windows-x64-setup.exe on Windows x64, or calabi-desktop-macos-universal.pkg on macOS. The macOS package includes both Apple Silicon and Intel components, the daemon, native remote desktop runtime and Finder integration.

Install over the existing application to retain the existing application identifiers, device identity, credentials and preferences. Windows, Android and macOS upgrade acceptance was reported successful by the maintainer. Desktop remote sessions, SSH/SFTP and file transfer continue to require the installed desktop application.

The browser console reports core 2.8.0; the native application reports desktop 3.0.0. These are separate versions.

The Windows installer and macOS package are not distributed with Windows Authenticode or Apple Developer ID signing/notarization in this preparation. Operating-system publisher verification prompts may appear.

## Verify downloads

Check the installer against SHA256SUMS. build-manifest.json and its detached signature record the product source, pinned core and installer hashes.

Source repository: https://github.com/calabinet/calabi-desktop
Product source commit: `3ad929a2c57d5f2095b78f895da9ce21e037916d`.
Bundled core: **2.8.0**, commit `62012a64fbe522105216b60eaee4c7529f8c6c7f`.
