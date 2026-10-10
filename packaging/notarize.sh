#!/usr/bin/env bash
# Notarise and staple the installer packages.
#
# Signing and notarising are two different things and both are needed.  Signing proves who built the package;
# notarisation is Apple scanning it and issuing a ticket, and since Catalina it is what decides whether a downloaded
# package opens or is refused with "Apple could not verify it is free of malware".  A signed-but-not-notarised package
# is still refused, so this step is not optional for anything that will be downloaded.
#
# Credentials live in a keychain profile, created once with:
#
#   xcrun notarytool store-credentials AquaType \
#     --apple-id <apple-id> --team-id <team-id> --password <app-specific-password>
#
# An app-specific password is made at appleid.apple.com, not the account password.
#
#   packaging/notarize.sh dist/AquaType-<stamp>-<commit>
set -euo pipefail

PROFILE="${AQUATYPE_NOTARY_PROFILE:-AquaType}"
DIR="${1:-}"
[[ -n "$DIR" && -d "$DIR" ]] || { echo "usage: notarize.sh <dist/AquaType-...>" >&2; exit 2; }
log() { echo "[notarize] $*"; }

shopt -s nullglob
PKGS=("$DIR"/*.pkg)
[[ ${#PKGS[@]} -gt 0 ]] || { echo "no .pkg in $DIR" >&2; exit 1; }

# Fail early and legibly.  The two things that actually go wrong here are an unsigned package (nothing to notarise) and
# an expired Apple Developer Program agreement, which returns a 403 that says nothing about what to do next.
for p in "${PKGS[@]}"; do
  if ! pkgutil --check-signature "$p" 2>/dev/null | grep -q "Developer ID Installer"; then
    echo "refusing: $(basename "$p") is not signed with a Developer ID Installer certificate." >&2
    echo "          Build it again - build_pkg.sh signs automatically when the certificate is in the keychain." >&2
    exit 1
  fi
done

if ! OUT="$(xcrun notarytool history --keychain-profile "$PROFILE" 2>&1)"; then
  echo "$OUT" >&2
  case "$OUT" in
    *"required agreement"*|*"403"*)
      cat >&2 <<'MSG'

Apple is refusing the submission because the Developer Program agreement has not been accepted.
Nothing in this repository can fix that - it needs a person to log in:

  1. https://developer.apple.com/account  ->  sign in
  2. Accept whatever agreement is shown (Apple reissues them; a lapsed one blocks notarisation
     while leaving the certificates themselves valid)
  3. Run this script again

MSG
      ;;
    *"No Keychain password item"*)
      cat >&2 <<MSG

No credentials stored under the profile "$PROFILE".  Create them once:

  xcrun notarytool store-credentials $PROFILE \\
    --apple-id <apple-id> --team-id <team-id> --password <app-specific-password>

MSG
      ;;
  esac
  exit 1
fi

# Stapling is retried.  `notarytool --wait` returns as soon as a submission is Accepted, and the ticket reaches the
# database `stapler` reads a little after that; stapling in that gap fails with Error 65 ("Could not find base64
# encoded ticket"), which reads like a rejected package and is not one.  Failing there would also stop the script
# between the two packages, leaving one un-stapled and the other never submitted.
staple_with_retry() {
  local p="$1" attempt
  for attempt in 1 2 3 4 5 6; do
    if xcrun stapler staple "$p"; then return 0; fi
    log "staple attempt $attempt failed (the ticket is usually not published yet); retrying in 30s"
    sleep 30
  done
  echo "could not staple $p after 6 attempts" >&2
  return 1
}

for p in "${PKGS[@]}"; do
  log "submitting $(basename "$p")"
  xcrun notarytool submit "$p" --keychain-profile "$PROFILE" --wait
  log "stapling $(basename "$p")"
  staple_with_retry "$p"
done

# The check that matters is the one Gatekeeper itself runs, against the installer policy rather than the default one.
for p in "${PKGS[@]}"; do
  log "$(basename "$p"): $(spctl -a -vv -t install "$p" 2>&1 | tr '\n' ' ')"
done

# The checksums people compare a download against, of the packages as they are now: stapling rewrote them.
(cd "$DIR" && shasum -a 256 -- *.pkg > SHA256SUMS.txt)
log "SHA256SUMS.txt: $(wc -l < "$DIR/SHA256SUMS.txt" | tr -d ' ') packages"

# Stapling rewrites the package, so the archive made at build time holds the pre-staple copy, and it is rebuilt here.
# A stapled package carries its ticket and clears Gatekeeper with no network; an un-stapled one has to reach Apple, and
# fails on a machine that is offline or behind a filter.
DIST_PARENT="$(cd "$DIR/.." && pwd)"; NAME="$(basename "$DIR")"; ZIP="$DIST_PARENT/$NAME.zip"
if [[ -f "$ZIP" ]]; then
  log "rebuilding $NAME.zip with the stapled packages"
  rm -f "$ZIP"
  (cd "$DIST_PARENT" && ditto -c -k --norsrc --keepParent "$NAME" "$ZIP")
  log "zip $ZIP ($(du -h "$ZIP" | awk '{print $1}'))"
fi
log "done - these packages now open without a Gatekeeper prompt on a machine that has never seen them"
