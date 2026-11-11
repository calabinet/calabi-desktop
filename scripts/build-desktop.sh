#!/usr/bin/env bash
# build-desktop.sh — full desktop release pipeline for Linux + macOS.
#
# Order of operations:
#   1. Build the calabi daemon binary for the host platform — or, when a macOS
#      --target is given, for that target's arch(es), lipo'd into one fat
#      Mach-O for universal (so the Tauri build can bake it in).
#   2. Build the embedded SPA (npm run build) — required for the daemon
#      to embed the latest UI.
#   3. Rebuild the daemon AFTER step 2 so the binary contains the fresh
#      SPA dist.
#   4. Copy the daemon binary into apps/client-desktop/ where
#      tauri.conf.json's bundle.resources expects it.
#   5. cargo tauri build — produces installers under
#      apps/client-desktop/src-tauri/target/release/bundle/.
#
# Args:
#   --skip-spa     Skip step 2 (use whatever's in ui/dist already).
#   --target NAME  Pass through to `cargo tauri build --target NAME`.
#                  For macOS targets the calabi daemon is ALSO built for the
#                  matching arch(es) and baked in, so universal-apple-darwin
#                  yields a real fat app (lipo'd daemon) instead of an arm64
#                  daemon smuggled into the Intel slice. The universal case
#                  needs `lipo` (Xcode Command Line Tools).
#
# Run from the repo root.

set -euo pipefail
# Windows builds share the preflight/lease; other hosts keep their native paths.
if [[ "${OSTYPE:-}" == msys* || "${OSTYPE:-}" == cygwin* ]]; then
  source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/dev/build-guard.sh" || exit $?
fi

SKIP_SPA=0
TARGET_ARG=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --skip-spa) SKIP_SPA=1; shift ;;
    --target) TARGET_ARG="--target $2"; shift 2 ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

if [[ -f product-build.json ]]; then
  PYTHON_CMD=python3
  command -v "$PYTHON_CMD" >/dev/null 2>&1 || PYTHON_CMD=python
  if [[ "$("$PYTHON_CMD" -c 'import json; print(json.load(open("product-build.json"))["local_override"])')" == False ]]; then
    SKIP_SPA=1
    echo "Using the pinned core console bundle"
  fi
fi
echo "==> 1/4 SPA build"
if [[ $SKIP_SPA -eq 0 ]]; then
  (cd apps/client/internal/status/ui && npm ci --no-audit --no-fund && npm run build)
else
  echo "    (skipped via --skip-spa)"
fi

echo "==> 2/4 calabi daemon build"
# Single source of truth: bin/calabi at repo root, matching the
# Makefile's BIN_DIR convention.
mkdir -p bin

# Fail fast with a clear message if the Go toolchain isn't visible to THIS
# shell (a common surprise when the script runs non-interactively or under a
# version manager whose shims aren't initialized).
command -v go >/dev/null 2>&1 || {
  echo "go not found in PATH for this shell." >&2
  echo "  Add Go's bin dir to PATH, e.g.: export PATH=\$PATH:/usr/local/go/bin" >&2
  echo "  (or init your version manager — goenv/asdf/gvm — non-interactively)." >&2
  exit 1
}

# The bundled daemon is baked into the desktop binary (build.rs include_bytes),
# so for production it MUST be a release build — else the shipped app runs a
# daemon pointing at localhost with the committed DEV edge CA. Mirror
# scripts/package-release-client.sh's bake when EDGE_CA is set; else warn.
VERSION="$(tr -d '[:space:]' < apps/client-desktop/VERSION)"
CORE_VERSION="$(tr -d '[:space:]' < VERSION)"
export CALABI_BUNDLED_CORE_VERSION="$CORE_VERSION"
PYTHON_CMD=python3
command -v "$PYTHON_CMD" >/dev/null 2>&1 || PYTHON_CMD=python
CALABI_BUNDLED_CORE_COMMIT="$("$PYTHON_CMD" -B scripts/products/product.py core-commit desktop)"
export CALABI_BUNDLED_CORE_COMMIT

# Keep the bundle version aligned with apps/client-desktop/VERSION. The installer (.msi/.nsis) version comes from
# tauri.conf.json — NOT the baked daemon's -ldflags version — so without this
# the bundle ships a stale version even though the daemon inside is $VERSION
# (mismatched installer, and a red-line collision if it reuses a published
# version). Cargo.toml's package version is kept aligned too. Idempotent; the
# edits are real (committed with the release), not reverted like the edge-CA.
TAURI_CONF="apps/client-desktop/src-tauri/tauri.conf.json"
CARGO_TOML="apps/client-desktop/src-tauri/Cargo.toml"
if [[ "$VERSION" != "dev" ]]; then
  # tauri.conf.json has exactly one "version" key (top-level).
  sed -i.bak -E 's/("version"[[:space:]]*:[[:space:]]*")[^"]*(")/\1'"$VERSION"'\2/' "$TAURI_CONF" && rm -f "$TAURI_CONF.bak"
  # Cargo.toml: only the [package] version sits at column 0 ("^version = ");
  # dependency version specs are indented or inline-table, so this is safe.
  sed -i.bak -E 's/^version = "[^"]*"/version = "'"$VERSION"'"/' "$CARGO_TOML" && rm -f "$CARGO_TOML.bak"
  echo "    synced desktop bundle version -> $VERSION (tauri.conf.json + Cargo.toml)"
fi
BFF_CONSOLE="${BFF_CONSOLE:-https://api.calabi.net}"
# The SAME baked defaults package-release-client.sh uses — this script
# COMPILES its own daemon (the Windows one re-uses the released binary), so
# every -X missing here ships a dev default inside the .pkg. It used to ship
# two: defaultConsoleWeb (login page linking to 127.0.0.1:5173) and
# defaultServer (a default edge of localhost:7443 where the release bakes it
# EMPTY so edge discovery runs instead). scripts/lint/check-release-ldflags.mjs
# now fails CI when this set drifts from the release packager's.
CONSOLE_WEB="${CONSOLE_WEB:-https://console.calabi.net}"
# Empty on purpose, like the release: no baked edge, discovered via bff-console.
EDGE_DEFAULT="${EDGE_DEFAULT:-}"
EMBED_CA="apps/client/internal/transport/certs/edge-ca.pem"
RELEASE=0
if [[ -n "${EDGE_CA:-}" ]]; then
  [[ -f "$EDGE_CA" ]] || { echo "EDGE_CA not found: $EDGE_CA" >&2; exit 1; }
  grep -q "BEGIN CERTIFICATE" "$EDGE_CA" || { echo "EDGE_CA is not a PEM certificate: $EDGE_CA" >&2; exit 1; }
  if grep -qE "BEGIN ((RSA|EC) )?PRIVATE KEY" "$EDGE_CA"; then
    echo "refusing: EDGE_CA contains a PRIVATE KEY — pass the public ca.crt only." >&2; exit 1
  fi
  if cmp -s "$EDGE_CA" "$EMBED_CA"; then
    echo "refusing: EDGE_CA is byte-identical to the committed DEV CA ($EMBED_CA)." >&2; exit 1
  fi
  echo "    RELEASE daemon: core=$CORE_VERSION desktop=$VERSION bff=$BFF_CONSOLE console=$CONSOLE_WEB edge-CA=$EDGE_CA"
  echo "    baking default edge: ${EDGE_DEFAULT:-<none — discovered via bff-console>}"
  ca_backup="$(mktemp)"
  cp -p "$EMBED_CA" "$ca_backup"
  trap 'cp -p "$ca_backup" "$EMBED_CA"; rm -f "$ca_backup"' EXIT
  cp -f "$EDGE_CA" "$EMBED_CA"
  RELEASE=1
else
  echo "    WARNING: no EDGE_CA — building a DEV daemon (version=dev, bff=localhost, DEV CA). NOT for production." >&2
  echo "             For a release: EDGE_CA=deploy/compose/data/edge-certs/ca.crt scripts/build-desktop.sh" >&2
fi

# Build the calabi daemon into repo-root-relative $2. $1 is GOARCH: empty => a
# host build (GOOS/GOARCH untouched, so this stays correct when the script runs
# on Linux); non-empty => a darwin cross-build for that arch. CGO is off in every
# case — the client is pure Go (matches the release path and every other client
# build script in the repo), which is also what lets the darwin cross-compile
# need no C toolchain.
build_daemon() {
  local goarch="$1" out="$2"
  # Bare `go` (shell command lookup, like the original inline form) — NOT
  # `env … go`, whose raw PATH exec misses a `go` provided via a shell
  # shim/function and dies with "env: go: No such file or directory". Env vars
  # are exported inside the subshell so they don't leak to the rest of the run.
  (
    cd apps/client
    export CGO_ENABLED=0
    if [[ -n "$goarch" ]]; then
      export GOOS=darwin GOARCH="$goarch"
    fi
    if [[ $RELEASE -eq 1 ]]; then
      go build -trimpath -buildvcs=false \
        -ldflags="-s -w -X main.version=$CORE_VERSION -X main.desktopProductVersion=$VERSION -X main.defaultBFFConsole=$BFF_CONSOLE -X main.defaultConsoleWeb=$CONSOLE_WEB -X main.defaultServer=$EDGE_DEFAULT" \
        -o "../../$out" ./cmd/calabi
    else
      go build -o "../../$out" ./cmd/calabi
    fi
  )
}

# The .pkg installs ONE daemon binary for both architectures, so on a universal
# target it has to be a fat Mach-O — a host-only build would leave Intel Macs
# with an arm64 binary the LaunchDaemon cannot exec. Build the matching arch(es)
# and lipo them together.
DAEMON_ARCHES=()   # empty => host build (unchanged default behavior)
case "${TARGET_ARG:-${CARGO_BUILD_TARGET:-}}" in
  *universal-apple-darwin*) DAEMON_ARCHES=(arm64 amd64) ;;
  *x86_64-apple-darwin*)    DAEMON_ARCHES=(amd64) ;;
  *aarch64-apple-darwin*)   DAEMON_ARCHES=(arm64) ;;
esac

if [[ ${#DAEMON_ARCHES[@]} -eq 0 ]]; then
  build_daemon "" bin/calabi
  echo "    wrote bin/calabi (canonical, host arch)"
elif [[ ${#DAEMON_ARCHES[@]} -eq 1 ]]; then
  build_daemon "${DAEMON_ARCHES[0]}" bin/calabi
  echo "    wrote bin/calabi (canonical, darwin/${DAEMON_ARCHES[0]})"
else
  command -v lipo >/dev/null 2>&1 || {
    echo "lipo not found — a universal daemon needs Xcode Command Line Tools (macOS)." >&2; exit 1; }
  slices=()
  for a in "${DAEMON_ARCHES[@]}"; do
    build_daemon "$a" "bin/calabi-$a"
    slices+=("bin/calabi-$a")
  done
  lipo -create -output bin/calabi "${slices[@]}"
  echo "    wrote bin/calabi (canonical, fat: $(lipo -archs bin/calabi))"
fi

# Copy into the Cargo release dir so the supervisor's sibling-binary
# fallback (resolve_binary in daemon.rs) finds it when the user runs
# `cargo tauri build` next. This copy is build-script-managed -- do
# NOT edit it by hand, it gets clobbered every build.
TAURI_BIN_DIR="apps/client-desktop/src-tauri/target/release"
mkdir -p "$TAURI_BIN_DIR"
cp -f bin/calabi "$TAURI_BIN_DIR/calabi"
echo "    copied to $TAURI_BIN_DIR/calabi (tauri sibling fallback)"

# Stage the daemon for the DEV sibling-fallback path.
#
# It is no longer baked into the exe: build.rs writes an EMPTY include_bytes!
# payload on purpose (the installer delivers the daemon, the app
# attaches to the machine-wide service), which is what keeps ~20MB of duplicated
# daemon out of the bundle. The staged copy is still what a DEBUG build resolves
# to as a sibling binary. Nothing here reaches the shipped .pkg: the daemon in
# the package is the one pkgbuild installs at /Library/Application Support.
EMBED_DIR="apps/client-desktop/src-tauri/binaries"
mkdir -p "$EMBED_DIR"
cp -f bin/calabi "$EMBED_DIR/calabi"
echo "    staged to $EMBED_DIR/calabi (baked into the desktop binary)"

# Tauri-build refuses to compile without icons. Auto-generate the
# placeholder set the first time.
if [[ ! -f apps/client-desktop/src-tauri/icons/icon.ico ]]; then
    echo "==> Icons missing — running gen-desktop-icons.sh"
    "$(dirname "${BASH_SOURCE[0]}")/gen-desktop-icons.sh"
fi

echo "==> 3/4 cargo tauri build"
if [[ "$(uname -s)" == Darwin && -n "${EDGE_CA:-}" ]]; then
  # Build only the .app here. A DMG created by Tauri at this point would wrap
  # the app before the RDP runtime and Finder extension have been installed.
  target="${TARGET_ARG#--target }"
  target="${target:-${CARGO_BUILD_TARGET:-}}"
  args=(--bundles app --features rdp-native-macos)
  if [[ -n "$target" ]]; then args+=(--target "$target"); fi
  tauri="$ROOT/apps/client-desktop/src-tauri"
  target_root="${CARGO_TARGET_DIR:-$tauri/target}"
  [[ "$target_root" == /* ]] || { echo "Use an absolute CARGO_TARGET_DIR." >&2; exit 1; }
  bundle="$target_root/${target:+$target/}release/bundle/macos"
  (cd "$tauri" && cargo tauri build "${args[@]}")
  bash "$ROOT/scripts/bundle-macos-rdp.sh" "$bundle/Calabi.app" "$target"
  echo "==> 4/4 app ready for package-macos-pkg.sh: $bundle/Calabi.app"
else
  (cd apps/client-desktop/src-tauri && cargo tauri build $TARGET_ARG)
  echo "==> 4/4 bundle output"
  ls -lh "apps/client-desktop/src-tauri/target/release/bundle/" 2>/dev/null || true
fi
