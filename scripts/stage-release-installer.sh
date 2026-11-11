#!/usr/bin/env bash
# stage-release-installer.sh - put an installer that was built on ANOTHER machine
# into dist/release/ the way the local build scripts put theirs there.
#
# The macOS .pkg has to be built on a Mac; the release is assembled on the
# Windows release machine, which holds the update signing key. Carrying the pkg
# across used to be three manual steps, and each one failed quietly when missed:
#
#   1. the published name. package-macos-pkg.sh writes Calabi-<VERSION>.pkg; the
#      download site serves calabi-desktop-macos-universal.pkg.
#   2. SHA256SUMS. Nothing listed the pkg, so publish-github-release.sh (which
#      stages from SHA256SUMS) would leave it out without an error.
#   3. latest.json. The manifest URL has to be the PUBLISHED name, or every
#      self-update fetches a file the site does not have.
#
# Usage:
#   scripts/stage-release-installer.sh <file> <published-name> [--platform <key>]
#
#   --platform <key>   also sign it into dist/release/latest.json under that key
#                      (darwin-universal for the pkg). Needs CALABI_UPDATE_PRIVKEY
#                      and CALABI_DOWNLOAD_BASE, like build-desktop.ps1.
#
# Example (Git Bash on the release machine):
#   CALABI_UPDATE_PRIVKEY=.secrets/calabi_desktop_private.key \
#   CALABI_DOWNLOAD_BASE=https://download.calabi.net/latest \
#     scripts/stage-release-installer.sh ~/Downloads/Calabi-1.12.0.pkg \
#       calabi-desktop-macos-universal.pkg --platform darwin-universal
set -euo pipefail

cd "$(dirname "$0")/.."

fail() { printf '\033[31mFAIL: %s\033[0m\n' "$*" >&2; exit 1; }

[ $# -ge 2 ] || fail "usage: $0 <file> <published-name> [--platform <key>]"
SRC="$1"; NAME="$2"; shift 2
PLATFORM=""
while [ $# -gt 0 ]; do
  case "$1" in
    --platform) [ $# -ge 2 ] || fail "--platform needs a key"; PLATFORM="$2"; shift 2 ;;
    *) fail "unknown argument: $1" ;;
  esac
done

[ -f "$SRC" ] || fail "no such file: $SRC"
case "$NAME" in */*|*\\*|"") fail "published name must be a bare file name: $NAME" ;; esac
VERSION="$(tr -d '[:space:]' < apps/client-desktop/VERSION)"
OUT="dist/release-desktop"
mkdir -p "$OUT"

if [ -n "$PLATFORM" ]; then
  : "${CALABI_UPDATE_PRIVKEY:?--platform needs CALABI_UPDATE_PRIVKEY (the update signing key)}"
  : "${CALABI_DOWNLOAD_BASE:?--platform needs CALABI_DOWNLOAD_BASE (e.g. https://download.calabi.net/latest)}"
  [ -f "$CALABI_UPDATE_PRIVKEY" ] || fail "CALABI_UPDATE_PRIVKEY not found: $CALABI_UPDATE_PRIVKEY"
  # Absolute before anything runs: the signer runs in scripts/updatekit, where a
  # relative key path would resolve against the wrong directory.
  KEY="$(cd "$(dirname "$CALABI_UPDATE_PRIVKEY")" && pwd)/$(basename "$CALABI_UPDATE_PRIVKEY")"
fi

cp "$SRC" "$OUT/$NAME"
HASH="$(sha256sum "$OUT/$NAME" | awk '{print $1}')"

# Replace this file's line, or add it. LF only: sha256sum -c reads a CR as part
# of the file name (release-pitfalls-and-verification.md 4).
TMP="$(mktemp)"
{ [ -f "$OUT/SHA256SUMS" ] && awk -v n="$NAME" '$2 != n' "$OUT/SHA256SUMS" || true
  printf '%s  %s\n' "$HASH" "$NAME"; } > "$TMP"
mv "$TMP" "$OUT/SHA256SUMS"
echo "staged $OUT/$NAME"
echo "  sha256 $HASH (recorded in $OUT/SHA256SUMS)"

if [ -n "$PLATFORM" ]; then
  URL="${CALABI_DOWNLOAD_BASE%/}/$NAME"
  echo "==> updatekit merge ($PLATFORM -> $OUT/latest.json, $URL)"
  ABS_OUT="$(cd "$OUT" && pwd)"
  ( cd scripts/updatekit && go run . merge --manifest "$ABS_OUT/latest.json" --version "$VERSION" \
      --platform "$PLATFORM" --installer "$ABS_OUT/$NAME" --url "$URL" --privkey "$KEY" )
fi

echo
echo "next: once every installer is staged, scripts/sync-packaging.sh, then (cd $OUT && sha256sum -c SHA256SUMS)"
