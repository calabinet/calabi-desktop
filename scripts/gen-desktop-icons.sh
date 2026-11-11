#!/usr/bin/env bash
# gen-desktop-icons.sh -- Linux/macOS counterpart of gen-desktop-icons.ps1.
#
# Uses the same Go program (scripts/gen-source-icon) to mint the
# source PNG, then `cargo tauri icon` for the variant generation.
#
# If you have a real designed 1024x1024 PNG, pass it as $1 and skip
# the procedural placeholder.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ICON_DIR="$ROOT/apps/client-desktop/src-tauri/icons"
mkdir -p "$ICON_DIR"

SRC="${1:-$ICON_DIR/source.png}"

if [[ ! -f "$SRC" ]]; then
    echo "==> Generating 1024x1024 placeholder PNG via Go"
    (cd "$ROOT/scripts/gen-source-icon" && go run . -out "$SRC")
else
    echo "==> Using existing source: $SRC"
fi

echo "==> cargo tauri icon $SRC"
(cd "$ROOT/apps/client-desktop/src-tauri" && cargo tauri icon "$SRC")

echo
echo "==> Generated icons:"
ls -lh "$ICON_DIR"/*.{png,ico,icns} 2>/dev/null || true
