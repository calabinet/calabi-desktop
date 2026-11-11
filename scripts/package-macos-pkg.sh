#!/usr/bin/env bash
# package-macos-pkg.sh — assemble a macOS installer .pkg that installs the Calabi
# desktop app AND registers the daemon as a root LaunchDaemon.
#
# This of. It produces
# an UNSIGNED pkg by default (testable without Apple certs). Pass --sign / --notarize
# for a distributable one (增量2). Run on macOS — needs pkgbuild/productbuild.
#
# Layout the pkg installs:
#   /Applications/Calabi.app                              (the GUI shell)
#   /Library/Application Support/Calabi/bin/calabi        (the daemon binary)
#   /Library/Calabi/bin/calabi                            (ordinary-user helper)
#   /Library/LaunchDaemons/com.calabi.daemon.plist        (root LaunchDaemon)
# postinstall (bulk) bootstraps the LaunchDaemon so the service runs immediately.
#
# Auth model: the service runs as root and serves the console at :7400 with NO
# baked credential — the user logs in through that console after install, and the
# root daemon persists the session to its SystemDataDir. So the plist is minimal.
#
# Args:
#   --app PATH     Path to the built Calabi.app (default: auto-discover under the
#                  tauri target/**/release/bundle/macos/).
#   --daemon PATH  Path to the calabi daemon binary (default: bin/calabi).
#   --sign         Sign the pkg with $CALABI_INSTALLER_IDENTITY (Developer ID
#                  Installer: …) — required for distribution / Gatekeeper.
#   --notarize     After signing, submit to notarytool + staple. Needs
#                  $APPLE_ID / $APPLE_TEAM_ID / $APPLE_PASSWORD (app-specific).
#
# Run from the repo root.

set -euo pipefail

case "$(uname -s)" in
  Darwin) : ;;
  *) echo "package-macos-pkg.sh: macOS only (needs pkgbuild/productbuild)." >&2; exit 1 ;;
esac

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

APP=""
DAEMON="bin/calabi"
OUTPUT_ARG=""
DO_SIGN=0
DO_NOTARIZE=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --app) APP="$2"; shift 2 ;;
    --daemon) DAEMON="$2"; shift 2 ;;
    --out) OUTPUT_ARG="$2"; shift 2 ;;
    --sign) DO_SIGN=1; shift ;;
    --notarize) DO_SIGN=1; DO_NOTARIZE=1; shift ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done

# Identifiers + paths. The daemon bundle id (com.calabi.daemon) is deliberately
# distinct from the app's (com.calabi.desktop) — see the plan doc.
PKG_IDENTIFIER="com.calabi.desktop"
DAEMON_LABEL="com.calabi.daemon"
DAEMON_INSTALL_DIR="/Library/Application Support/Calabi/bin"
FILE_AGENT_INSTALL_DIR="/Library/Calabi/bin"
PLIST_INSTALL_PATH="/Library/LaunchDaemons/${DAEMON_LABEL}.plist"

VERSION="$(tr -d '[:space:]' < apps/client-desktop/VERSION 2>/dev/null || echo 0.0.0)"

# Auto-discover the built .app if not given. Prefer a universal build.
if [[ -z "$APP" ]]; then
  for cand in \
    "apps/client-desktop/src-tauri/target/universal-apple-darwin/release/bundle/macos/Calabi.app" \
    "apps/client-desktop/src-tauri/target/release/bundle/macos/Calabi.app"; do
    if [[ -d "$cand" ]]; then APP="$cand"; break; fi
  done
fi
[[ -n "$APP" && -d "$APP" ]] || { echo "Calabi.app not found — build it first (scripts/build-desktop.sh) or pass --app." >&2; exit 1; }
[[ -f "$DAEMON" ]] || { echo "daemon binary not found: $DAEMON (build it or pass --daemon)." >&2; exit 1; }

universal=0
if lipo "$DAEMON" -verify_arch arm64 && lipo "$DAEMON" -verify_arch x86_64; then
  universal=1
  bash "$ROOT/scripts/verify-macos-rdp-bundle.sh" "$APP"
fi

echo "==> app:    $APP"
echo "==> daemon: $DAEMON"
echo "==> version: $VERSION"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
PAYLOAD="$WORK/payload"     # mirrors the install tree (pkgbuild --root)
SCRIPTS="$WORK/scripts"     # pre/postinstall
mkdir -p "$PAYLOAD/Applications" \
         "$PAYLOAD${DAEMON_INSTALL_DIR}" \
         "$PAYLOAD${FILE_AGENT_INSTALL_DIR}" \
         "$PAYLOAD/Library/LaunchDaemons" \
         "$SCRIPTS"

# 1. App bundle → /Applications
cp -R "$APP" "$PAYLOAD/Applications/"

# 2. Daemon binary → /Library/Application Support/Calabi/bin/calabi
cp -f "$DAEMON" "$PAYLOAD${DAEMON_INSTALL_DIR}/calabi"
chmod 755 "$PAYLOAD${DAEMON_INSTALL_DIR}/calabi"

# The service data directory is root-only. Keep an identical executable outside
# it for the ordinary-user file agent, without changing the signed app bundle.
cp -f "$DAEMON" "$PAYLOAD${FILE_AGENT_INSTALL_DIR}/calabi"
chmod 755 "$PAYLOAD/Library/Calabi" "$PAYLOAD${FILE_AGENT_INSTALL_DIR}" \
          "$PAYLOAD${FILE_AGENT_INSTALL_DIR}/calabi"

# 3. LaunchDaemon plist. Minimal: run the daemon as root with the system-service
#    marker so it uses SystemDataDir + logs where postinstall can read it.
cat > "$PAYLOAD${PLIST_INSTALL_PATH}" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key><string>${DAEMON_LABEL}</string>
  <key>ProgramArguments</key>
  <array>
    <string>${DAEMON_INSTALL_DIR}/calabi</string>
    <string>daemon</string>
  </array>
  <key>EnvironmentVariables</key>
  <dict>
    <key>CALABI_SYSTEM_SERVICE</key><string>1</string>
  </dict>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><true/>
  <key>StandardOutPath</key><string>/Library/Application Support/Calabi/daemon.log</string>
  <key>StandardErrorPath</key><string>/Library/Application Support/Calabi/daemon.log</string>
</dict>
</plist>
PLIST

# 4. postinstall: fix ownership + (re)bootstrap the LaunchDaemon so the service
#    is running the moment the install finishes. Idempotent across upgrades.
cat > "$SCRIPTS/postinstall" <<'POST'
#!/bin/bash
set -e
LABEL="com.calabi.daemon"
PLIST="/Library/LaunchDaemons/${LABEL}.plist"
mkdir -p "/Library/Application Support/Calabi"
chown root:wheel "/Library/Application Support/Calabi"
chmod 700 "/Library/Application Support/Calabi"
# Repair these exact binary paths on upgrades; never broaden the credential
# directory or recursively change ownership of service/user data.
chown root:wheel "/Library/Calabi" "/Library/Calabi/bin" "/Library/Calabi/bin/calabi"
chmod 755 "/Library/Calabi" "/Library/Calabi/bin" "/Library/Calabi/bin/calabi"
chown root:wheel "$PLIST"
chmod 644 "$PLIST"
# Register/enable the menu for the actual console user, not Installer's root user.
# No logged-in GUI user: the ordinary-user helper will attempt this on first use.
MENU_ACTIVATE="/Applications/Calabi.app/Contents/MacOS/CalabiRemoteFiles.app/Contents/Resources/activate-menu.sh"
if [ -f "$MENU_ACTIVATE" ]; then
  console_user=$(/usr/bin/stat -f %Su /dev/console 2>/dev/null || true)
  case "$console_user" in
    ''|root|loginwindow|_mbsetupuser) ;;
    *) console_uid=$(/usr/bin/id -u "$console_user" 2>/dev/null || true)
       if [ -n "$console_uid" ] && [ "$console_uid" -ge 501 ]; then
         /bin/launchctl asuser "$console_uid" /usr/bin/sudo -H -u "$console_user" \
           /bin/bash "$MENU_ACTIVATE" || echo 'postinstall: Finder menu activation deferred to first use' >&2
       fi ;;
  esac
fi
# On upgrade the old instance is loaded (on a self-update it is the daemon that
# started this installer): take it down, then load the freshly installed
# plist/binary.
#
# bootout can return while launchd is still tearing the job down, and a
# bootstrap that lands in that gap fails ("Bootstrap failed: 5: Input/output
# error"). Under set -e that ends this script with the service unloaded, and
# KeepAlive cannot help a job that is no longer loaded. So wait until the old
# job is gone, and retry the bootstrap before giving up.
if launchctl print "system/${LABEL}" >/dev/null 2>&1; then
  launchctl bootout "system/${LABEL}" 2>/dev/null || true
  for _ in $(seq 1 60); do
    launchctl print "system/${LABEL}" >/dev/null 2>&1 || break
    sleep 0.5
  done
fi
# enable BEFORE bootstrap: a service someone disabled refuses to bootstrap.
launchctl enable "system/${LABEL}" 2>/dev/null || true
for attempt in 1 2 3 4 5; do
  launchctl bootstrap system "$PLIST" && exit 0
  echo "postinstall: bootstrap attempt ${attempt} failed" >&2
  sleep 2
done
exit 1
POST
chmod 755 "$SCRIPTS/postinstall"

# 5. Build a component pkg, then wrap in a product archive (distribution).
COMPONENT="$WORK/calabi-component.pkg"
DIST_DIR="$ROOT/dist"
mkdir -p "$DIST_DIR"
OUT="${OUTPUT_ARG:-$DIST_DIR/Calabi-${VERSION}.pkg}"
mkdir -p "$(dirname "$OUT")"
# The name the download site serves it under (web/www Download.tsx). A self-update
# manifest has to point at THIS name, not at the local file's.
PUBLISHED_NAME="calabi-desktop-macos-universal.pkg"

# Bundle relocation OFF. pkgbuild marks every .app in the payload relocatable
# by default, and installer then writes the app over ANY copy of the same bundle
# id it finds, instead of into /Applications. On a machine that builds the app
# that copy is the tauri output in the source tree: both the manual install and
# the self-update went there, and the self-update — started by the root
# LaunchDaemon, which has no access to ~/Documents — died on "Operation not
# permitted" before postinstall ever ran (2026-09-16). The daemon's path is
# fixed; the app's is too.
COMPONENT_PLIST="$WORK/component.plist"
pkgbuild --analyze --root "$PAYLOAD" "$COMPONENT_PLIST"
n=0
while /usr/libexec/PlistBuddy -c "Print :$n" "$COMPONENT_PLIST" >/dev/null 2>&1; do
  /usr/libexec/PlistBuddy -c "Set :$n:BundleIsRelocatable false" "$COMPONENT_PLIST"
  n=$((n + 1))
done
# Zero entries = analyze found no bundle, so the loop switched nothing off. Build
# anyway and the pkg ships relocatable again with nobody the wiser.
if [[ $n -lt 1 ]]; then
  echo "pkgbuild --analyze found no bundle to pin — refusing to build a relocatable pkg." >&2
  exit 1
fi

echo "==> pkgbuild"
pkgbuild \
  --root "$PAYLOAD" \
  --component-plist "$COMPONENT_PLIST" \
  --scripts "$SCRIPTS" \
  --identifier "$PKG_IDENTIFIER" \
  --version "$VERSION" \
  --install-location "/" \
  "$COMPONENT"

# Check the RESULT, not the plist fed in: a <bundle> inside <relocate> in the
# built PackageInfo is exactly what broke the update.
pkgutil --expand "$COMPONENT" "$WORK/component-expanded"
RELOCATE="$(sed -n '/<relocate>/,/<\/relocate>/p' "$WORK/component-expanded/PackageInfo")"
if grep -q '<bundle' <<<"$RELOCATE"; then
  echo "the built component still relocates a bundle:" >&2
  echo "$RELOCATE" >&2
  exit 1
fi

echo "==> productbuild"
DISTRIBUTION="$WORK/Distribution"
# Stage only localized resources. A leftover global Welcome.html from older
# preview source patches would take precedence over every localized copy.
RESOURCES="$WORK/installer-resources"
mkdir -p "$RESOURCES"
for language in en zh-Hans zh-Hant; do
  cp -R "$ROOT/scripts/macos-installer-resources/$language.lproj" "$RESOURCES/"
done
productbuild --synthesize --package "$COMPONENT" "$WORK/Distribution.generated"
if ! awk '
  !added && sub(/<installer-gui-script([[:space:]][^>]*)?>/, "&\n    <welcome file=\"Welcome.html\" mime-type=\"text/html\"/>") { added=1 }
  { print }
  END { if (!added) exit 1 }
' "$WORK/Distribution.generated" > "$DISTRIBUTION"; then
  echo "Could not add the welcome page to the generated Installer distribution." >&2
  exit 1
fi
product_args=(--distribution "$DISTRIBUTION" --package-path "$WORK" --resources "$RESOURCES")
if [[ $DO_SIGN -eq 1 ]]; then
  : "${CALABI_INSTALLER_IDENTITY:?--sign needs \$CALABI_INSTALLER_IDENTITY (Developer ID Installer: …)}"
  product_args+=(--sign "$CALABI_INSTALLER_IDENTITY")
fi
productbuild "${product_args[@]}" "$OUT"

if [[ $universal -eq 1 ]]; then
  echo "==> verify extracted universal pkg"
  pkgutil --expand-full "$OUT" "$WORK/product-expanded"
  extracted="$WORK/product-expanded/calabi-component.pkg/Payload"
  bash "$ROOT/scripts/verify-macos-rdp-bundle.sh" "$extracted/Applications/Calabi.app"
  cmp "$DAEMON" "$extracted${DAEMON_INSTALL_DIR}/calabi"
  cmp "$DAEMON" "$extracted${FILE_AGENT_INSTALL_DIR}/calabi"
fi

if [[ $DO_NOTARIZE -eq 1 ]]; then
  : "${APPLE_ID:?--notarize needs \$APPLE_ID}"; : "${APPLE_TEAM_ID:?\$APPLE_TEAM_ID}"; : "${APPLE_PASSWORD:?\$APPLE_PASSWORD}"
  echo "==> notarytool submit (wait)"
  xcrun notarytool submit "$OUT" --apple-id "$APPLE_ID" --team-id "$APPLE_TEAM_ID" --password "$APPLE_PASSWORD" --wait
  echo "==> staple"
  xcrun stapler staple "$OUT"
fi

# Optional: sign the pkg into a self-update manifest entry. Skipped unless
# BOTH the signing key and the public download base are set; a dev build just
# produces the installer.
if [[ -n "${CALABI_UPDATE_PRIVKEY:-}" && -n "${CALABI_DOWNLOAD_BASE:-}" ]]; then
  # Same manifest path build-desktop.ps1 uses, so mac + win merge into ONE
  # latest.json (carry it between the platform builds).
  MANIFEST="$DIST_DIR/release-desktop/latest.json"
  URL="${CALABI_DOWNLOAD_BASE%/}/$PUBLISHED_NAME"
  echo "==> updatekit merge (darwin-universal -> $MANIFEST)"
  ( cd "$ROOT" && go run ./scripts/updatekit merge \
      --manifest "$MANIFEST" --version "$VERSION" --platform darwin-universal \
      --installer "$OUT" --url "$URL" --privkey "$CALABI_UPDATE_PRIVKEY" )
fi

echo
echo "Done: $OUT"
[[ $DO_SIGN -eq 0 ]] && echo "(UNSIGNED — for local testing. Distribute with --sign/--notarize.)"
echo "Install/test:  sudo installer -pkg \"$OUT\" -target /"
# The published name says universal; a single-arch build must not go out under it.
if [[ $universal -eq 1 ]]; then
  echo "Release:       carry it to the release machine and run"
  echo "               scripts/stage-release-installer.sh <this pkg> $PUBLISHED_NAME --platform darwin-universal"
else
  echo "NOT for release: the daemon is $(lipo -archs "$DAEMON" 2>/dev/null || echo '?'), not universal (arm64 + x86_64)."
fi
