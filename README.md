# Calabi Desktop

Native Calabi desktop application for Windows and macOS. The application bundles a tested Calabi daemon, remote desktop components and file transfer helpers.

The product version is `apps/client-desktop/VERSION`. The shared core is maintained in [calabinet/calabi](https://github.com/calabinet/calabi); `apps/client-desktop/core-source.json` pins its version and complete public commit. Core source is fetched for builds, rather than maintained in this repository.

## Build

Install Python 3.12+, Git, Go 1.25+, Node.js 20+, Rust with the Tauri CLI, and the platform's native build tools. Windows needs MSVC and WebView2. macOS needs Xcode Command Line Tools and the native dependencies documented in the RDP helper source under `tools/rdp-p0`.

```sh
python scripts/products/product.py prepare desktop --destination ../calabi-desktop-build
cd ../calabi-desktop-build
```

On Windows use PowerShell 7:

```powershell
pwsh -NoProfile -File scripts/build-desktop.ps1
```

On macOS, reuse previously downloaded pinned source archives (OpenSSL 3.6.5 and FreeRDP 3.33.0) from your local directory:

```sh
export CALABI_RDP_DOWNLOAD_DIR="$HOME/Downloads"
export CALABI_RDP_OFFLINE=1
EDGE_CA="$PWD/calabi-edge-ca.pem" bash scripts/build-desktop.sh --target universal-apple-darwin
bash scripts/package-macos-pkg.sh --daemon bin/calabi
```

Offline mode verifies each archive's fixed SHA-256 and never downloads. Missing or mismatched archives stop the build and preserve existing files. To allow a missing archive to download, unset `CALABI_RDP_OFFLINE`. Without an explicit download directory, verified downloads persist in `$HOME/Library/Caches/Calabi/rdp-sources` across builds; scratch SDK builds are separate and cleaned after bundling. Production builds need your deployment's public CA at the `EDGE_CA` path above.

Builds without a supplied deployment Edge CA are development builds. For a production build, pass the deployment's **public** CA through `-EdgeCa` (Windows) or `EDGE_CA` (macOS). Keep signing keys and passwords outside the source tree. Existing application identifiers, service paths and credential stores are preserved.

The native application displays its own version. The local browser console displays the version of its actual running daemon. Settings list the product and core versions separately.

## Release preparation

Build from a clean public product checkout and the pinned core. Windows stages its installer in `dist/release-desktop`; carry the macOS package to the same release workspace and use `scripts/stage-release-installer.sh` to add it. Update manifests use the existing signing key and desktop channel; the two platforms must be combined before publishing.

```sh
python scripts/products/product.py manifest desktop
```

This records the product version, core version, complete source commits and installer hashes. Development `--core-tree` overrides are explicitly marked and refused for release metadata. Publishing tags, installers, signatures and update manifests is a separate release operation.

## Source and licensing

Calabi source is licensed under Apache-2.0; see `LICENSE` and `NOTICE`. RDP and SSH dependencies have their own notices and pinned build recipes under the source directories. Preserve these notices in redistributions. Native codec availability and platform signing requirements must be verified for each distribution.


## Verify release provenance before publishing

From the prepared public build, sign the product manifest with the established release key (kept outside Git), then preview the release:

```powershell
pwsh -NoProfile -File scripts/dev/build.ps1 go run ./scripts/updatekit sign-file --file dist/release-desktop/build-manifest.json --privkey <private-key-file> > dist/release-desktop/build-manifest.json.sig
python scripts/products/publish-release.py desktop
```

The preview verifies the manifest signature, installer hashes, product version, fixed core and complete installer set. `--publish` additionally requires an existing public tag at the recorded product source commit. Release tags and publication are separate operations. Desktop update descriptors retain the established desktop signing key and channel.


## Prepared source archive

From the prepared build, create a source archive for another build machine:

```sh
python scripts/products/product.py archive desktop --destination ../calabi-desktop-source.zip --edge-ca <public-ca.crt>
```

The archive includes only recorded public source inputs, their provenance and the optional public CA. It works without a Git checkout and refuses development overrides, changed inputs or private keys. For a formal desktop release, build Windows and macOS from the same selected product source commit; validation installers are separate from release publication.
