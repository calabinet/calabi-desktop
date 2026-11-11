#!/usr/bin/env bash
# Fetch a pinned archive once; retain verified downloads across build directories.
calabi_fetch_source() (
  set -euo pipefail
  name="${1:?Pass archive name}"
  url="${2:?Pass archive URL}"
  digest="${3:?Pass SHA-256}"
  cache="${CALABI_RDP_DOWNLOAD_DIR:-$HOME/Library/Caches/Calabi/rdp-sources}"
  case "$name" in */*|*\\*|.|..) echo "Invalid source archive name" >&2; exit 1 ;; esac
  verify() {
    if command -v sha256sum >/dev/null 2>&1; then
      printf '%s  %s\n' "$digest" "$1" | sha256sum -c - >&2
    else
      printf '%s  %s\n' "$digest" "$1" | shasum -a 256 -c - >&2
    fi
  }
  archive="$cache/$name"
  if [[ -f "$archive" ]]; then
    echo "==> Reusing local source archive: $archive" >&2
    verify "$archive" || { echo "Source checksum mismatch; local file preserved: $archive" >&2; exit 1; }
    printf '%s\n' "$archive"
    exit 0
  fi
  if [[ "${CALABI_RDP_OFFLINE:-0}" == 1 ]]; then
    echo "Missing local source archive: $archive (offline mode; no download attempted)" >&2
    exit 1
  fi
  mkdir -p "$cache" || exit 1
  partial="$(mktemp "$cache/.${name}.part.XXXXXX")" || exit 1
  trap 'rm -f "$partial"' EXIT
  echo "==> Downloading pinned source archive: $name" >&2
  curl --fail --show-error --progress-bar --location --connect-timeout 20 \
    --max-time 900 --speed-limit 1024 --speed-time 60 --retry 3 "$url" -o "$partial" || exit 1
  verify "$partial" || { echo "Downloaded source checksum mismatch: $name" >&2; exit 1; }
  # Another build may have populated the cache while this download was running.
  # Never replace an existing local archive, even if its checksum is incorrect.
  mv -n "$partial" "$archive" || exit 1
  verify "$archive" || exit 1
  echo "==> Source archive cached: $archive" >&2
  printf '%s\n' "$archive"
)
