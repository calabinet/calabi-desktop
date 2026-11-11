# Calabi desktop app

A window and a tray icon for the `calabi` client, built with Tauri 2. The window
shows the client's local console (`http://127.0.0.1:7400`) — the same page a
browser shows. The app has no web code of its own.

Released for Windows (a setup `.exe`) and macOS (a `.pkg`).

## How it works

- **The installer installs the client as a system service.** The Windows setup
  ships `calabi.exe` beside the app (`bundle.resources` in
  `src-tauri/tauri.windows.conf.json`) and registers it with
  `calabi daemon install --system` (`src-tauri/nsis-hooks.nsh`). The macOS
  `.pkg` installs the app and the client as a LaunchDaemon. The service runs the
  endpoints and the mesh whether the app is open or not.
- **The app attaches to the service.** It finds the service's console and shows
  it in the window; the tray shows the service's state, checked every five
  seconds. A release build never starts a daemon of its own.
- **Product updates come from the service.** The native window selects the desktop
  product's signed update channel; a plain browser selects the running core's
  channel. Both platform-connected and self-hosted modes support checking.
  The desktop-owned daemon is upgraded with the desktop package. The app itself
  has no separate updater.
- The installers are not code-signed yet: Windows SmartScreen and macOS
  Gatekeeper warn on first open.

## Build

Needs Rust (stable), Go 1.25+ and the Tauri 2 CLI
(`cargo install tauri-cli --version "^2"`), plus:

- Windows: the MSVC build tools and the WebView2 runtime (included in Windows 11);
- macOS: the Xcode command-line tools;
- Linux: `webkit2gtk-4.1-dev`, `libssl-dev`, `libgtk-3-dev`, `librsvg2-dev`,
  `libayatana-appindicator3-dev`.

Build from a clean [public desktop checkout](https://github.com/calabinet/calabi-desktop):

```powershell
python scripts/products/product.py prepare desktop --destination ../calabi-desktop-build
Set-Location ../calabi-desktop-build
pwsh -NoProfile -File scripts/build-desktop.ps1
```

The product version is `apps/client-desktop/VERSION`; `core-source.json` fixes the
public core version and complete commit. The prepared build supplies the daemon,
shared console and notices. macOS packaging and production-CA instructions are in
the public repository's root README. Formal builds use the pinned core console
bundle and the committed Cargo.lock.

### Running it while developing

From the prepared build on Windows:

```powershell
Set-Location apps/client-desktop/src-tauri
pwsh -NoProfile -File ../../../scripts/dev/build.ps1 cargo tauri dev
```

On macOS, use `cargo tauri dev` in that same directory.

A debug build attaches to a running service, like a release build. When there
is none, it starts a daemon itself, looking for `calabi` in this order:

1. `CALABI_DAEMON_PATH` — set it for the current shell only
   (`$env:CALABI_DAEMON_PATH = "$PWD\bin\calabi.exe"`); release builds ignore it;
2. next to the app;
3. `PATH`.

Or start `calabi daemon` in another terminal first.

## Layout

```
apps/client-desktop/
├── shell/index.html           # shown until the console answers
├── tests/nsis-hooks/          # tests for the installer hooks (Windows)
└── src-tauri/
    ├── tauri.conf.json        # the app; tauri.windows.conf.json adds the installer settings
    ├── nsis-hooks.nsh         # Windows installer: register and remove the service
    ├── binaries/              # where the build expects calabi(.exe)
    ├── capabilities/          # what the window may do
    ├── icons/
    └── src/
        ├── main.rs            # window, tray, single instance, start at login
        └── daemon.rs          # finding and attaching to the service
```
