# Changelog

## 3.1.0 — 2026-11-14

- **Added:** Recent Remote cards display locally saved Windows desktop and Linux SSH terminal thumbnails. Session switching also shows current SSH terminal contents.
- **Added:** Windows fullscreen SSH sessions share the top-edge connection bar, with minimize and end-session controls.
- **Changed:** Sign-in and two-step verification use separate steps within the same compact window; the main window expands after authentication.
- **Changed:** File-transfer certificates are maintained automatically when safe. Routine certificate details and the separate Transfer Center settings panel are hidden.
- **Fixed:** Windows remote desktop uses the remote computer's input method. Physical Shift state is synchronized so shifted punctuation works when a modifier transition is missed.
- **Fixed:** Desktop thumbnails can cross the local agent channel without increasing the request limit for ordinary operations.

Install over the existing desktop application to retain device identity, credentials and preferences. The bundled core remains 2.8.0 with an updated source commit. Windows x64 and macOS universal installers are updated; standalone core packages and their update channel are unchanged.

## Unreleased

- **Added:** Self-hosted deployments can use desktop RDP to sign in to approved Windows devices in the same mesh, using Windows credentials and the existing access rules and pinned certificates. Platform user ownership is not required for this self-hosted authorization.

Upgrade the self-hosted coordinator and both desktop endpoints. Published 3.0.1 installers do not include this change.

## 3.0.1 — 2026-11-13

- **Added:** Light, dark and system appearance options, available from the account menu.
- **Added:** An auto-hiding fullscreen connection bar on Windows, with minimize and end-session controls.
- **Changed:** Redesigned tray menus and compact remote login cards, with rounded borders and improved window depth.
- **Changed:** More compact navigation and overview layouts, improved support for larger Windows text, and thin scrollbars that appear while scrolling.
- **Fixed:** Windows remote desktop sessions could stop processing input after an extended connection because of a native timer overflow.
- **Fixed:** Remote login requests could appear unresponsive while waiting for the local agent or an existing session.
- **Fixed:** Windows screenshots copied to the clipboard can be pasted into a remote Windows session.
- **Fixed:** Devices republish their remote desktop certificate after re-registering.
- **Added:** Remote session diagnostics, stall detection and logs using the client system's local date and time.

Install over the existing application to retain device identity, credentials and preferences. The desktop product is 3.0.1; the bundled core version remains 2.8.0 with an updated source commit. This release does not update Android or standalone core packages.

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
