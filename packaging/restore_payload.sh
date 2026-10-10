#!/usr/bin/env bash
# Restore the payload binaries that are kept out of git - the model weights, the frequency shortlist and the fallback
# dictionary store - from a released AquaType-<version>.pkg, and check them against packaging/payload/SHA256SUMS.
#
# The package carries both byte for byte (checked 2026-09-23), so the release that holds the package is also the
# backup of the build inputs; nothing else has to be stored separately.
#
#   packaging/restore_payload.sh <AquaType-<version>.pkg>   restore from a package on disk
#   packaging/restore_payload.sh --release <tag>            download the installer from that GitHub release first (gh)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT
if [[ "${1:-}" == "--release" ]]; then
  [[ -n "${2:-}" ]] || { echo "usage: restore_payload.sh --release <tag>" >&2; exit 2; }
  gh release download "$2" --repo dio7153/AquaType --pattern "AquaType-*[0-9].pkg" --dir "$WORK"   # not the -Uninstall one
  PKG="$(ls "$WORK"/AquaType-*.pkg | head -1)"
else
  PKG="${1:?usage: restore_payload.sh <AquaType-<version>.pkg> | --release <tag>}"
fi
pkgutil --expand-full "$PKG" "$WORK/x" >/dev/null
APP="$(find "$WORK/x" -maxdepth 4 -name AquaType.app -type d | head -1)"
[[ -n "$APP" ]] || { echo "no AquaType.app inside $PKG" >&2; exit 1; }
mkdir -p "$ROOT/packaging/payload/decoder" "$ROOT/packaging/payload/dict_store"
cp "$APP/Contents/Resources/AquaTypeDecoder/model.safetensors" "$ROOT/packaging/payload/decoder/model.safetensors"
cp "$APP/Contents/Resources/AquaTypeDecoder/norm_rank.bin" "$ROOT/packaging/payload/decoder/norm_rank.bin"
cp "$APP/Contents/Resources/AquaTypeData/dictionary_fallback.v1.aqds" "$ROOT/packaging/payload/dict_store/dictionary_fallback.v1.aqds"
(cd "$ROOT/packaging/payload" && shasum -a 256 -c SHA256SUMS)
