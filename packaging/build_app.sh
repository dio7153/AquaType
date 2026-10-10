#!/usr/bin/env bash
# Build AquaType.app - the input method (a Squirrel fork), the engine it runs as a child process, the native core and
# MLX runtime, the decoder export and the data - check it, sign it, and hand it to build_pkg.sh for the installer and
# the uninstaller.  Output: dist/<name>/ and dist/<name>.zip.  Nothing in the result needs Python or a daemon.
#   packaging/build_app.sh [--decoder-dir DIR] [--rime-payload DIR] [--skip-app-build]
#
# Needs Xcode, Swift, and a .venv in the repository root with mlx installed (its version is the MLX the package ships).
# The model files are not in git: packaging/restore_payload.sh copies them out of a released package.
# Environment: AQUATYPE_DECODER_DIR, AQUATYPE_RIME_PAYLOAD and AQUATYPE_DICT_STORE override the three payload directories
# (default packaging/payload/decoder, rime and dict_store); AQUATYPE_CODESIGN_IDENTITY names the signing identity
# (default: the keychain's Developer ID Application identity, or ad hoc when there is none).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
STAMP="$(date +%Y%m%d_%H%M%S)"; COMMIT="$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"
NAME="AquaType-${STAMP}-${COMMIT}"; DIST="$ROOT/dist"; OUT="$DIST/$NAME"
# Build inputs live in the repository, not in ~/.aquatype: that is a runtime directory, and uninstalling removes it.
DECODER_DIR="${AQUATYPE_DECODER_DIR:-$ROOT/packaging/payload/decoder}"; RIME_PAYLOAD="${AQUATYPE_RIME_PAYLOAD:-$ROOT/packaging/payload/rime}"; DICT_STORE="${AQUATYPE_DICT_STORE:-$ROOT/packaging/payload/dict_store}"
SIGN="${AQUATYPE_CODESIGN_IDENTITY:--}"; SKIP_APP_BUILD=0
VERSION="0.9.0"   # the release version people see; the build number below changes on every build
while [[ $# -gt 0 ]]; do case "$1" in --decoder-dir) DECODER_DIR="$2"; shift 2;; --rime-payload) RIME_PAYLOAD="$2"; shift 2;; --skip-app-build) SKIP_APP_BUILD=1; shift;; *) echo "unknown arg $1" >&2; exit 2;; esac; done
need() { [[ -e "$1" ]] || { echo "missing: $1" >&2; exit 1; }; }
log() { echo "[build_app] $*"; }

need "$DECODER_DIR/model.safetensors"; need "$DECODER_DIR/model.json"; need "$DECODER_DIR/tables.safetensors"; need "$DECODER_DIR/tables.json"
# norm_rank.bin is the frequency shortlist the normaliser sums over, and the order the third source ranks its
# characters by.  Shipping without it does not fail: the decoder quietly falls back to the exact normaliser and
# the character list to code-point order, so the build would simply be worse than the one that was measured.
need "$DECODER_DIR/norm_rank.bin"
need "$DICT_STORE/dictionary_fallback.v1.aqds"; need "$DICT_STORE/dictionary_fallback.v1.aqds.manifest.json"; need "$ROOT/data/char_to_keys.json"
for f in aquatype_bopomofo.schema.yaml squirrel.custom.yaml default.custom.yaml aquatype.extended.dict.yaml aquatype_lexicon.dict.yaml aquatype_boost.dict.yaml aquatype_mcbopomofo.dict.yaml terra_pinyin.dict.yaml aquatype_phrases.dict.yaml aquatype_variants.dict.yaml; do need "$RIME_PAYLOAD/$f"; done
# The export must be the format the engine reads: the tag aq_open dispatches on.
grep -q '"format": "aquatype-v2/1"' "$DECODER_DIR/model.json" || { echo "refusing: decoder export is not aquatype-v2/1" >&2; exit 1; }
MLX_LIB="$("$ROOT/.venv/bin/python" -c 'import mlx.core, os; print(os.path.join(os.path.dirname(os.path.dirname(mlx.core.__file__)), "mlx", "lib"))')"
need "$MLX_LIB/libmlx.dylib"; need "$MLX_LIB/mlx.metallib"

# ---- the oldest macOS this build runs on ---------------------------------------------------------------------------
#
# 14.0 (Sonoma), on any Apple silicon Mac.  Every part is built for it explicitly; left to their defaults they each
# target something different, the newest being the Mac the build runs on.
#
# 14.0 is the floor MLX itself sets: mlx-metal publishes each release built for macOS 14, 15 and 26, and the macOS 14
# build gives the same candidates at the same speed.  The input method and the native core compile at 14.0 with no
# availability warnings; going lower would mean an older MLX.
MIN_MACOS="14.0"
export MACOSX_DEPLOYMENT_TARGET="$MIN_MACOS"

# The MLX runtime that ships: the same MLX release as the venv (so the headers the native core compiled against are
# the ones it runs with), but the build published for macOS 14 rather than the one pip picked for this Mac.  Fetched
# from PyPI once per version, checked against the sha256 PyPI publishes for it, and kept under .build.
MLX_VERSION="$("$ROOT/.venv/bin/python" -c 'import mlx.core as mx; print(mx.__version__)')"
MLX_RUNTIME_DIR="$ROOT/.build/mlx-runtime/mlx-metal-$MLX_VERSION-macosx_14_0_arm64"
MLX_RUNTIME="$MLX_RUNTIME_DIR/mlx/lib"
if [[ ! -f "$MLX_RUNTIME/libmlx.dylib" ]]; then
  log "fetching the macOS 14 build of mlx-metal $MLX_VERSION"
  mkdir -p "$MLX_RUNTIME_DIR"
  read -r WHEEL_URL WHEEL_SHA < <(curl -fsSL "https://pypi.org/pypi/mlx-metal/$MLX_VERSION/json" | python3 -c '
import json, sys
for f in json.load(sys.stdin)["urls"]:
    if f["filename"].endswith("-macosx_14_0_arm64.whl"):
        print(f["url"], f["digests"]["sha256"]); break')
  [[ -n "${WHEEL_URL:-}" ]] || { echo "refusing: PyPI has no macOS 14 build of mlx-metal $MLX_VERSION" >&2; exit 1; }
  curl -fsSL -o "$MLX_RUNTIME_DIR/mlx-metal.whl" "$WHEEL_URL"
  GOT_SHA="$(shasum -a 256 "$MLX_RUNTIME_DIR/mlx-metal.whl" | awk '{print $1}')"
  [[ "$GOT_SHA" == "$WHEEL_SHA" ]] || { echo "refusing: mlx-metal wheel sha256 $GOT_SHA, PyPI says $WHEEL_SHA" >&2; exit 1; }
  (cd "$MLX_RUNTIME_DIR" && unzip -q -o mlx-metal.whl)
fi
need "$MLX_RUNTIME/libmlx.dylib"; need "$MLX_RUNTIME/mlx.metallib"
log "MLX runtime: mlx-metal $MLX_VERSION, macOS 14 build (minos $(vtool -show-build "$MLX_RUNTIME/libmlx.dylib" | awk '/minos/{print $2}'))"

log "building the native core (libaquadecoder)"
(cd "$ROOT" && bash decoder/native/build_dylib.sh >/dev/null)
need "$ROOT/decoder/native/libaquadecoder.dylib"
log "building the engine (release)"
(cd "$ROOT" && swift build -c release --product AquaTypeEngine >/dev/null)
ENGINE="$ROOT/.build/release/AquaTypeEngine"; need "$ENGINE"
if [[ $SKIP_APP_BUILD -eq 0 ]]; then
  log "building the input method app"
  # The bundle is assembled afresh each time: left in place, a resource the project no longer has (an upstream icon,
  # long removed) stays in it and ships.  The compiled objects are kept, so this costs no rebuild.
  rm -rf "$ROOT/third_party/squirrel/build/Build/Products/Release/AquaType.app" \
         "$ROOT/third_party/squirrel/build/Build/Intermediates.noindex/Squirrel.build/Release/Squirrel.build/assetcatalog_output"
  xcodebuild -project "$ROOT/third_party/squirrel/Squirrel.xcodeproj" -scheme Squirrel -configuration Release -jobs 4 -derivedDataPath "$ROOT/third_party/squirrel/build" CODE_SIGNING_ALLOWED=NO MACOSX_DEPLOYMENT_TARGET="$MIN_MACOS" build >/dev/null
fi
APP_BUILD="$ROOT/third_party/squirrel/build/Build/Products/Release/AquaType.app"; need "$APP_BUILD/Contents/MacOS/Squirrel"

rm -rf "$OUT"; mkdir -p "$OUT"
APP="$OUT/AquaType.app"; ditto "$APP_BUILD" "$APP"; RES="$APP/Contents/Resources"
# A build number that changes every build, stamped before anything is signed.  macOS keys an input method's cached
# metadata - the menu icon, the mode list, where its resources are - on the bundle identifier and the version, so a new
# bundle under an old version keeps the old record: typing works, while the entry in the input menu is stale and
# vanishes when clicked.  The build stamp is unique per build and sorts, so it is the build number.
BUILD_NUMBER="${STAMP%%_*}.${STAMP##*_}"
/usr/libexec/PlistBuddy -c "Set :CFBundleVersion $BUILD_NUMBER" "$APP/Contents/Info.plist" >/dev/null 2>&1 \
  || /usr/libexec/PlistBuddy -c "Add :CFBundleVersion string $BUILD_NUMBER" "$APP/Contents/Info.plist" >/dev/null
/usr/libexec/PlistBuddy -c "Set :CFBundleShortVersionString $VERSION" "$APP/Contents/Info.plist" >/dev/null 2>&1 \
  || /usr/libexec/PlistBuddy -c "Add :CFBundleShortVersionString string $VERSION" "$APP/Contents/Info.plist" >/dev/null
/usr/libexec/PlistBuddy -c "Set :NSHumanReadableCopyright Copyright © 2026 Dio Hsieh" "$APP/Contents/Info.plist" >/dev/null 2>&1 \
  || /usr/libexec/PlistBuddy -c "Add :NSHumanReadableCopyright string Copyright © 2026 Dio Hsieh" "$APP/Contents/Info.plist" >/dev/null
log "bundle version $(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$APP/Contents/Info.plist") build $BUILD_NUMBER"
# rime_deployer and rime_dict_manager are upstream's command-line tools.  Nothing in AquaType runs them - the schema
# is built by `Squirrel --build` - so they are not shipped.
rm -f "$RES/README.md" "$APP/Contents/MacOS/rime-install" "$APP/Contents/MacOS/rime_deployer" "$APP/Contents/MacOS/rime_dict_manager"
# Upstream also ships other input methods' schemas and dictionaries, OpenCC's data, a grammar model and three librime
# plugins (Lua, predict, octagram).  AquaType builds one schema, which needs the five base files below, and essay.txt
# is the word-frequency list librime's preset vocabulary is read from; the rest is removed, so the bundle carries
# nothing it does not run - and no third-party data it would have to account for without using it.
for f in "$APP/Contents/SharedSupport"/*; do
  case "$(basename "$f")" in
    default.yaml|squirrel.yaml|zhuyin.yaml|key_bindings.yaml|punctuation.yaml|essay.txt) ;;
    *) rm -rf "$f" ;;
  esac
done
rm -rf "$APP/Contents/Frameworks/rime-plugins"
/usr/libexec/PlistBuddy -c "Set :LSMinimumSystemVersion $MIN_MACOS" "$APP/Contents/Info.plist" >/dev/null 2>&1 \
  || /usr/libexec/PlistBuddy -c "Add :LSMinimumSystemVersion string $MIN_MACOS" "$APP/Contents/Info.plist" >/dev/null
log "embedding the engine, native core, MLX runtime, decoder and data"
mkdir -p "$RES/lib" "$RES/AquaTypeDecoder" "$RES/AquaTypeData" "$RES/rime"
install -m 0755 "$ENGINE" "$RES/AquaTypeEngine"
install -m 0755 "$ROOT/decoder/native/libaquadecoder.dylib" "$RES/lib/libaquadecoder.dylib"
install -m 0755 "$MLX_RUNTIME/libmlx.dylib" "$RES/lib/libmlx.dylib"
install -m 0644 "$MLX_RUNTIME/mlx.metallib" "$RES/lib/mlx.metallib"
# every @rpath dependency of the MLX runtime ships too (the macOS 26 build links libjaccl.dylib; the macOS 14 build
# does not, but this stays so that a runtime which does is complete rather than unloadable on any other machine)
for dep in $(otool -L "$MLX_RUNTIME/libmlx.dylib" | awk '/@rpath\//{print $1}' | sed 's|@rpath/||' | grep -v '^libmlx.dylib$'); do
  [[ -f "$MLX_RUNTIME/$dep" ]] || { echo "refusing: MLX runtime dependency $dep not found next to libmlx" >&2; exit 1; }
  install -m 0755 "$MLX_RUNTIME/$dep" "$RES/lib/$dep"; log "MLX dependency embedded: $dep"
done

# Nothing in the bundle may need a newer macOS than the one it says it supports.  This is what makes the minimum a
# fact instead of a hope: an MLX upgrade, or a tool built without a deployment target, would otherwise raise it
# silently.
TOO_NEW=""
while IFS= read -r -d '' m; do
  file -b "$m" | grep -q "Mach-O" || continue
  v="$(vtool -show-build "$m" 2>/dev/null | awk '/minos/{print $2; exit}')"
  [[ -n "$v" ]] || continue
  if [[ "$(printf '%s\n%s\n' "$v" "$MIN_MACOS" | sort -V | tail -1)" != "$MIN_MACOS" ]]; then TOO_NEW+="  $v  ${m#$APP/}"$'\n'; fi
done < <(find "$APP" -type f -print0)
[[ -z "$TOO_NEW" ]] || { printf 'refusing: these need a newer macOS than %s:\n%s' "$MIN_MACOS" "$TOO_NEW" >&2; exit 1; }
log "every binary in the bundle runs on macOS $MIN_MACOS or later"
install_name_tool -add_rpath "@loader_path" "$RES/lib/libmlx.dylib" 2>/dev/null || true
for f in model.safetensors model.json tables.safetensors tables.json norm_rank.bin; do install -m 0644 "$DECODER_DIR/$f" "$RES/AquaTypeDecoder/$f"; done
install -m 0644 "$DICT_STORE/dictionary_fallback.v1.aqds" "$RES/AquaTypeData/dictionary_fallback.v1.aqds"
install -m 0644 "$DICT_STORE/dictionary_fallback.v1.aqds.manifest.json" "$RES/AquaTypeData/dictionary_fallback.v1.aqds.manifest.json"
install -m 0644 "$ROOT/data/char_to_keys.json" "$RES/AquaTypeData/char_to_keys.json"
# Licences and attributions: the GPL text Squirrel already ships (Resources/LICENSE.txt) plus what AquaType adds.
mkdir -p "$RES/licenses"
install -m 0644 "$APP/Contents/Resources/LICENSE.txt" "$RES/licenses/LICENSE-GPL-3.0.txt"
for f in NOTICE.txt THIRD-PARTY-LICENSES.txt LICENSE-Apache-2.0.txt LICENSE-CC-BY-NC-4.0.txt LICENSE-MLX.txt LICENSE-LGPL-3.0.txt; do install -m 0644 "$ROOT/packaging/legal/$f" "$RES/licenses/$f"; done
for f in aquatype_bopomofo.schema.yaml squirrel.custom.yaml default.custom.yaml aquatype.extended.dict.yaml aquatype_lexicon.dict.yaml aquatype_boost.dict.yaml aquatype_mcbopomofo.dict.yaml terra_pinyin.dict.yaml aquatype_phrases.dict.yaml aquatype_variants.dict.yaml; do install -m 0644 "$RIME_PAYLOAD/$f" "$RES/rime/$f"; done
# the engine and the core find their dylibs next to the engine: Resources/lib -- and nowhere else: every absolute rpath the
# build left behind (Xcode toolchain, the dev tree, the venv's mlx) is removed, so the bundle can only load its own copies
# (with the dev paths first, the bundled engine on the build machine would load the dev tree's libraries and pass).
for bin in "$RES/AquaTypeEngine" "$RES/lib/libaquadecoder.dylib"; do
  for rp in $(otool -l "$bin" | awk '/LC_RPATH/{getline; getline; print $2}'); do
    case "$rp" in /*) install_name_tool -delete_rpath "$rp" "$bin" 2>/dev/null || true;; esac
  done
done
install_name_tool -add_rpath "@executable_path/lib" "$RES/AquaTypeEngine" 2>/dev/null || true
install_name_tool -add_rpath "@loader_path" "$RES/lib/libaquadecoder.dylib" 2>/dev/null || true
# The debug map swift build leaves in the engine names the object files on the build machine, home folder included; it
# is no use to anyone the package reaches.  Every binary built here is checked for one.
strip -S "$RES/AquaTypeEngine"
for bin in "$APP/Contents/MacOS/Squirrel" "$RES/AquaTypeEngine" "$RES/lib/libaquadecoder.dylib"; do
  if nm -a "$bin" 2>/dev/null | grep -q " OSO "; then echo "refusing: $bin still names its build's object files" >&2; exit 1; fi
done
# self-check: one real request through the bundled engine (loads the core, MLX and its dependencies) -- every library must come from
# Resources/lib; a load failure or an outside path fails the build loudly
SC_STATE=$(mktemp -d); SC_OUT=$(DYLD_PRINT_LIBRARIES=1 "$RES/AquaTypeEngine" --selftest --decoder-dir "$RES/AquaTypeDecoder" --metallib "$RES/lib/mlx.metallib" --char-to-keys "$RES/AquaTypeData/char_to_keys.json" --state-dir "$SC_STATE" --fallback-aqds "$RES/AquaTypeData/dictionary_fallback.v1.aqds" --fallback-manifest "$RES/AquaTypeData/dictionary_fallback.v1.aqds.manifest.json" 2>&1) || { echo "refusing: bundled engine self-test failed:" >&2; echo "$SC_OUT" | grep -v "^dyld" | tail -5 >&2; exit 1; }
RES_REAL=$(cd "$RES" && pwd -P)                                                                 # dist/ may be a symlink: compare real paths
for lib in libaquadecoder.dylib libmlx.dylib $(ls "$RES/lib" | grep -E '^lib.*\.dylib$' | grep -v -e libaquadecoder -e libmlx); do
  got=$(echo "$SC_OUT" | grep "dyld" | grep "/$lib" | head -1)
  case "$got" in *"$RES_REAL/lib/$lib"*|*"$RES/lib/$lib"*) ;; *) echo "refusing: bundled engine loaded $lib from outside the bundle: ${got:-<not loaded>}" >&2; exit 1;; esac
done
echo "$SC_OUT" | grep -q '"candidates":\[{' || { echo "refusing: bundled engine self-test returned no candidates" >&2; echo "$SC_OUT" | grep -v "^dyld" | tail -3 >&2; exit 1; }
log "bundle self-check: one request answered; core, MLX and $(ls "$RES/lib" | grep -c dylib) dylibs loaded from Resources/lib"

# What Resources holds is exactly what this build puts there: a file left over from an earlier build of the project (the
# asset catalog keeps what it compiled once, an icon removed long ago among it) would otherwise ship unnoticed.
EXPECTED_RESOURCES="AquaType.icns AquaTypeData AquaTypeDecoder AquaTypeEngine Assets.car LICENSE.txt aquatype.pdf en.lproj lib licenses rime zh-Hans.lproj zh-Hant.lproj"
FOUND_RESOURCES="$(ls "$RES" | LC_ALL=C sort | tr '\n' ' ' | sed 's/ $//')"
[[ "$FOUND_RESOURCES" == "$EXPECTED_RESOURCES" ]] \
  || { echo "refusing: Contents/Resources holds [$FOUND_RESOURCES], expected [$EXPECTED_RESOURCES]" >&2; exit 1; }

# ---- signing ------------------------------------------------------------------------------------------------------
#
# Developer ID with the hardened runtime, signed inside out: every Mach-O individually, nested first and the bundle
# last, because that is the only order codesign accepts.  The entitlements are empty: neither the input method nor the
# engine connects to anything, and every library either loads is signed with the same identity, so library validation
# stays on.
if [[ "$SIGN" == "-" ]]; then
  FOUND="$(security find-identity -v -p codesigning 2>/dev/null | awk -F'"' '/Developer ID Application/ {print $2; exit}')"
  [[ -n "$FOUND" ]] && SIGN="$FOUND"
fi
ENTITLEMENTS="$ROOT/third_party/squirrel/resources/Squirrel.entitlements"
if [[ "$SIGN" == "-" ]]; then
  log "signing: ad hoc (no Developer ID Application identity in the keychain)"
  CS=(--force --sign -)
else
  log "signing: $SIGN"
  CS=(--force --options runtime --timestamp --sign "$SIGN")
fi
MAIN_EXE="$APP/Contents/MacOS/Squirrel"
while IFS= read -r m; do
  [[ "$m" == "$MAIN_EXE" ]] && continue          # signed as part of the bundle, below
  case "$(file -b "$m")" in
    *"Mach-O"*executable*) codesign "${CS[@]}" --entitlements "$ENTITLEMENTS" "$m" >/dev/null ;;
    *"Mach-O"*) codesign "${CS[@]}" "$m" >/dev/null ;;
  esac
done < <(find "$APP" \( -type f -perm +111 -o -name '*.dylib' \))
codesign "${CS[@]}" --entitlements "$ENTITLEMENTS" "$APP" >/dev/null
codesign --verify --deep --strict "$APP" >/dev/null
UNSIGNED="$(find "$APP" \( -type f -perm +111 -o -name '*.dylib' \) -exec sh -c 'file -b "$1" | grep -q Mach-O && ! codesign -v "$1" 2>/dev/null && echo "$1"' _ {} \; | head -5)"
[[ -z "$UNSIGNED" ]] || { echo "refusing: unsigned Mach-O left in the bundle:" >&2; echo "$UNSIGNED" >&2; exit 1; }

log "verifying the bundled engine answers from inside the bundle"
VERIFY_STATE="$(mktemp -d)"
"$RES/AquaTypeEngine" --check-char-coverage "$RES/AquaTypeData/char_to_keys.json" | grep -q '"ok":true'
# Answered through the decoder, and answered well: a real reading has its word at the top, so a model, table or decoder
# that answers with nothing, or with noise, is refused here rather than found by the person typing.
for check in "ji3|" "5j4u4|注意"; do
  RAW="${check%%|*}"; WANT="${check#*|}"
  "$RES/AquaTypeEngine" --selftest --raw "$RAW" --fallback-aqds "$RES/AquaTypeData/dictionary_fallback.v1.aqds" --fallback-manifest "$RES/AquaTypeData/dictionary_fallback.v1.aqds.manifest.json" --decoder-dir "$RES/AquaTypeDecoder" --metallib "$RES/lib/mlx.metallib" --char-to-keys "$RES/AquaTypeData/char_to_keys.json" --state-dir "$VERIFY_STATE" 2>"$VERIFY_STATE/err" >"$VERIFY_STATE/out" || true
  python3 - "$VERIFY_STATE/out" "$WANT" <<'PY' || { echo "refusing: the bundled engine's answer for $RAW is wrong:" >&2; cat "$VERIFY_STATE/err" >&2; exit 1; }
import json, sys
answer = json.load(open(sys.argv[1])); want = sys.argv[2]
texts = [c["text_utf8"] for c in answer.get("candidates", [])]
problems = []
if answer.get("visible_path") != "DECODER_V1": problems.append(f"visible_path {answer.get('visible_path')}, not DECODER_V1")
if not texts or not all(0x3400 <= ord(ch) <= 0x9FFF for ch in texts[0]): problems.append(f"first candidates {texts[:3]}")
if want and want not in texts[:3]: problems.append(f"{want} not in the first three: {texts[:3]}")
if problems: print("; ".join(problems), file=sys.stderr); sys.exit(1)
PY
done
rm -rf "$VERIFY_STATE"

# The engine runs for the whole login session, so a power assertion it holds keeps the Mac from sleeping for as long as
# AquaType is in use.  It is asked one question as the input method would, and the system's list of assertions is read
# while it waits for the next.
log "checking the engine lets the Mac sleep"
SLEEP_DIR="$(mktemp -d)"; mkfifo "$SLEEP_DIR/in"; mkdir "$SLEEP_DIR/state"
"$RES/AquaTypeEngine" --command serve-ui-jsonl --decoder-dir "$RES/AquaTypeDecoder" --metallib "$RES/lib/mlx.metallib" \
  --char-to-keys "$RES/AquaTypeData/char_to_keys.json" --state-dir "$SLEEP_DIR/state" \
  --fallback-aqds "$RES/AquaTypeData/dictionary_fallback.v1.aqds" \
  --fallback-manifest "$RES/AquaTypeData/dictionary_fallback.v1.aqds.manifest.json" \
  <"$SLEEP_DIR/in" >"$SLEEP_DIR/out" 2>"$SLEEP_DIR/err" &
ENGINE_PID=$!
exec 7>"$SLEEP_DIR/in"
trap '' PIPE                     # an engine that has already exited makes the write fail (|| true), not end this script
printf '%s\n' '{"schema":"aquatype-ui-fallback-request/v1","client_id":"build","client_generation":1,"raw_keys":"ji3","raw_identity":"amkz","committed_context_utf8":"","context_identity":"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855","max_candidates":9,"native_shell_candidates":[]}' >&7 || true
for _ in $(seq 1 300); do [[ -s "$SLEEP_DIR/out" ]] && break; sleep 0.1; done
ANSWERED="$([[ -s "$SLEEP_DIR/out" ]] && echo yes || echo no)"
HELD="$(pmset -g assertions | grep "pid $ENGINE_PID(" || true)"
exec 7>&-; trap - PIPE; kill "$ENGINE_PID" 2>/dev/null || true; wait "$ENGINE_PID" 2>/dev/null || true
ENGINE_ERR="$(tail -5 "$SLEEP_DIR/err")"; rm -rf "$SLEEP_DIR"
[[ "$ANSWERED" == yes ]] || { echo "refusing: the engine did not answer a request within 30 s:" >&2; echo "$ENGINE_ERR" >&2; exit 1; }
[[ -z "$HELD" ]] || { echo "refusing: the engine holds a power assertion, so the Mac cannot sleep while AquaType is in use:" >&2; echo "$HELD" >&2; exit 1; }

# How well and how fast the bundled engine answers, on twenty everyday sentences typed a key at a time: today 17 are
# first and all 20 in the first three, at a p95 of about 60 ms per key on an M4.  The limits leave room for a slower
# Mac and a busy one, and none for a model, table or decoder that has stopped working or slowed several times over.
log "checking the engine's answers and speed"
CHECK_OUT="$("$ROOT/.venv/bin/python" "$ROOT/packaging/check_engine.py" "$RES" --min-top1 16 --min-top3 20 --max-p95-ms 150 2>&1)" \
  || { echo "$CHECK_OUT" >&2; exit 1; }
log "  $(echo "$CHECK_OUT" | head -1)"

# The engine's own tests, run against what ships rather than the development build: the JSONL contract, the third
# source, and the personal memory.  None may fail, and none may be skipped for want of something.
log "running the engine's tests against the bundle"
TEST_OUT="$(AQUATYPE_TEST_ENGINE="$RES/AquaTypeEngine" AQUATYPE_TEST_AQDS="$RES/AquaTypeData/dictionary_fallback.v1.aqds" \
  AQUATYPE_TEST_DECODER="$RES/AquaTypeDecoder" AQUATYPE_TEST_METALLIB="$RES/lib/mlx.metallib" \
  "$ROOT/.venv/bin/python" -m pytest -q -rs -p no:cacheprovider "$ROOT/decoder/tests/test_engine.py" 2>&1)" \
  || { echo "refusing: the engine's tests failed against the bundle:" >&2; echo "$TEST_OUT" | tail -30 >&2; exit 1; }
if echo "$TEST_OUT" | grep -qi "skipped"; then echo "refusing: engine tests were skipped:" >&2; echo "$TEST_OUT" | tail -10 >&2; exit 1; fi
log "  $(echo "$TEST_OUT" | tail -1)"

# ---- the shell -----------------------------------------------------------------------------------------------------
#
# The input method itself: the Rime schema that turns keys into syllables, the panel that shows what came back, and
# typing through both.  A schema that does not build is an input method where no key produces a candidate, so this is
# checked here, before anything ships; the postinstall builds it again on the user's machine and refuses the same
# stale result.
log "building the Rime schema from the bundle's payload"
SHELL_CHECK="$(mktemp -d)"
cp "$RES"/rime/*.yaml "$SHELL_CHECK/"
"$APP/Contents/MacOS/Squirrel" --build "$SHELL_CHECK" "$APP/Contents/SharedSupport" "$SHELL_CHECK/build" \
  >"$SHELL_CHECK/build.log" 2>&1 || { echo "refusing: the Rime schema did not build" >&2; tail -20 "$SHELL_CHECK/build.log" >&2; exit 1; }
BUILT="$SHELL_CHECK/build/aquatype_bopomofo.schema.yaml"
[[ -f "$BUILT" && -f "$SHELL_CHECK/build/aquatype_bopomofo.prism.bin" ]] || { echo "refusing: $SHELL_CHECK/build is incomplete" >&2; tail -20 "$SHELL_CHECK/build.log" >&2; exit 1; }
# librime logs a failure and carries on, and `--build` still exits 0, so the files existing is not the same as the
# build having worked: the log is read for errors too.  glog writes `E<yyyymmdd> ...` for them.
if grep -qE '^E[0-9]{8} ' "$SHELL_CHECK/build.log"; then
  echo "refusing: the Rime deploy reported errors (the schema still built, which is why this is checked separately):" >&2
  grep -E '^E[0-9]{8} ' "$SHELL_CHECK/build.log" | head -10 >&2
  exit 1
fi
# A build left from an earlier version still has the parts this one dropped: librime's punctuator, recognizer and
# simplifier, and the full-width switch.  Every earlier build has them and this one has none, so finding one means the
# build is stale - the same list the postinstall checks.  Punctuation itself is checked below by --type-selftest, which
# types every punctuation key and compares the result with what macOS's built-in 注音 typed.
grep -q "delimiter:" "$BUILT" || { echo "refusing: the built schema has no speller delimiter" >&2; exit 1; }
for stale in '- punctuator' '- recognizer' '- simplifier' 'full_shape'; do
  if grep -qF -- "$stale" "$BUILT"; then
    echo "refusing: the built schema is stale: it still has $stale" >&2; exit 1
  fi
done
log "Rime schema built and checked"

# The candidate panel, laid out over the product's real cases.  Needs a window server session, and says so rather than
# passing when it cannot run - the grep below is for the positive line, so a skip fails the build.
log "laying out the candidate panel"
"$APP/Contents/MacOS/Squirrel" --render-selftest "$SHELL_CHECK" >"$SHELL_CHECK/render.log" 2>&1 || {
  echo "refusing: the candidate panel did not lay out" >&2; cat "$SHELL_CHECK/render.log" >&2; exit 1; }
grep -q '^render-selftest ok - ' "$SHELL_CHECK/render.log" || {
  echo "refusing: the candidate panel self-test did not report success" >&2; cat "$SHELL_CHECK/render.log" >&2; exit 1; }
log "candidate panel: $(grep '^render-selftest ok - ' "$SHELL_CHECK/render.log")"
# The candidates-in-a-column line: absent means nothing with more than one candidate was measured, which would make a
# pass meaningless, so it is required rather than merely printed.
grep -q '^render-selftest ok alignment: ' "$SHELL_CHECK/render.log" || {
  echo "refusing: the candidate panel self-test did not measure candidate alignment" >&2; exit 1; }
log "  $(grep '^render-selftest ok alignment: ' "$SHELL_CHECK/render.log" | sed 's/^render-selftest ok //')"

# Typing, through the real input controller and the bundled engine: whether a person can type with this package.  In four
# processes side by side - everything but the field matrix, and the field matrix in three parts - each with its own copy
# of the Rime directory, since librime locks its user database.  Every part has to report success.
log "typing through the input controller"
TYPE_PARTS=(main matrix:0/3 matrix:1/3 matrix:2/3)
TYPE_PIDS=()
for part in "${TYPE_PARTS[@]}"; do
  name=${part//[:\/]/_}
  cp -R "$SHELL_CHECK" "$SHELL_CHECK.$name"
  "$APP/Contents/MacOS/Squirrel" --type-selftest "$SHELL_CHECK.$name" "$part" >"$SHELL_CHECK.$name.log" 2>&1 &
  TYPE_PIDS+=($!)
done
TYPE_FAILED=0
for pid in "${TYPE_PIDS[@]}"; do wait "$pid" || TYPE_FAILED=1; done
for part in "${TYPE_PARTS[@]}"; do cat "$SHELL_CHECK.${part//[:\/]/_}.log"; done >"$SHELL_CHECK/type.log"
[[ $TYPE_FAILED -eq 0 ]] || {
  echo "refusing: typing did not work" >&2; grep -E '^type-selftest' "$SHELL_CHECK/type.log" >&2 || tail -20 "$SHELL_CHECK/type.log" >&2; exit 1; }
[[ $(grep -c '^type-selftest ok - ' "$SHELL_CHECK/type.log") -eq ${#TYPE_PARTS[@]} ]] || {
  echo "refusing: the typing self-test did not report success for every part" >&2; grep -E '^type-selftest' "$SHELL_CHECK/type.log" >&2; exit 1; }
grep -E '^type-selftest ok (commit|composition|length cap|no session|panel as drawn)' "$SHELL_CHECK/type.log" | while IFS= read -r line; do log "  $line"; done
for part in "${TYPE_PARTS[@]}"; do rm -rf "$SHELL_CHECK.${part//[:\/]/_}" "$SHELL_CHECK.${part//[:\/]/_}.log"; done
rm -rf "$SHELL_CHECK"


# The installer and the uninstaller, both packages (build_pkg.sh).
bash "$ROOT/packaging/build_pkg.sh" --bundle "$OUT/AquaType.app"
cat > "$OUT/README.txt" <<EOF
AquaType ${VERSION} 公開測試版

AquaType 是適用於 Mac 的注音輸入法，使用標準注音鍵盤（大千式）。輸入一整句注音後，
AquaType 會以在這台 Mac 上執行的語言模型轉換成中文；按錯鍵時，候選中也會列出修正後的句子。

系統需求
• 配備 Apple 晶片的 Mac（不支援 Intel 處理器的 Mac）。建議使用 M3 或後續晶片；
  在 M1、M2 上，候選會出現得比較慢。
• macOS Sonoma 14 或以上版本。

安裝
1. 打開「AquaType-${VERSION}.pkg」，依照畫面上的指示完成安裝。
2. 選擇「蘋果」選單 >「登出」，然後重新登入。macOS 會在登入時載入新安裝的輸入法。
3. 選擇「蘋果」選單 >「系統設定」，按一下側邊欄中的「鍵盤」，前往「文字輸入」，
   然後按一下「編輯」。按一下加入按鈕「+」，選取「繁體中文」，再選取右側列表中的
   「AquaType」，然後按一下「加入」。

使用技巧
輸入注音時，候選會列出整句的轉換結果。按 Tab 可以切換到「詞」和「單字」候選，
一段一段選字送出。

隱私權
AquaType 完全離線，只在這台 Mac 上執行，不會連線到網際網路，也不會傳送任何資料。
你送出的字詞會連同當時按下的按鍵（包括打錯的鍵）、次數和時間記錄在這台 Mac 上，
稱為「個人記憶」，用來調整候選的順序；最多保留 4,000 筆，較少用到的會先被淘汰。
為了選出更合適的字，AquaType 會讀取游標前最多 256 個字元作為上下文，這些文字只在
轉換時使用，不會被記錄。在密碼欄位等安全輸入狀態下，AquaType 不讀取也不記錄。
個人記憶存放在個人專屬檔案夾中的「.aquatype」檔案夾（在 Finder 按 Command-Shift-G，
輸入 ~/.aquatype）。若要清除，請刪除其中檔名以「personal_memory」開頭的檔案，
然後登出再重新登入。

移除
1. 先從選單列的輸入法選單切換到其他輸入法（例如 ABC 或注音）。
2. 打開「AquaType-${VERSION}-Uninstall.pkg」，依照畫面上的指示完成移除。AquaType 和目前使用者帳號中的
   設定、記錄檔、個人記憶都會刪除，其他輸入法不受影響。
3. 登出再重新登入，AquaType 就會從輸入法選單中消失。
EOF
# What the model came from: the export records the two checkpoints it was built from, under `keylm` and `charlm`.
# Not recorded is reported as that, which says nothing about whether the model is trained.
MODEL_SOURCE="$(python3 - "$DECODER_DIR/model.json" <<'PY' 2>/dev/null || echo "unknown (model.json could not be read)"
import json, sys
src = (json.load(open(sys.argv[1])).get("source") or {})
if src.get("keylm_sha256") or src.get("charlm_sha256"):          # a syllable model and a character model
    print(" + ".join(f"{name} step {src.get(name + '_step')} sha256 {(src.get(name + '_sha256') or '')[:16]}"
                     for name in ("keylm", "charlm") if src.get(name + "_sha256")))
else:
    print("not recorded by the exporter (this says nothing about whether the model is trained)")
PY
)"
# Everything between the EOFs below is literal - a `#` in there lands in the file, it does not comment anything out.
# `decoder_tables` is the digest of tables.json, which pins the decode parameters this build ships.
# A record for whoever builds, beside the folder people are handed rather than in it: which checkpoints and binaries
# this package holds, for the day a report names a build.
cat > "$DIST/$NAME-inventory.txt" <<EOF
model source: $MODEL_SOURCE
app=AquaType-${VERSION}.pkg
engine=Contents/Resources/AquaTypeEngine sha256=$(shasum -a 256 "$RES/AquaTypeEngine" | awk '{print $1}')
libaquadecoder=$(shasum -a 256 "$RES/lib/libaquadecoder.dylib" | awk '{print $1}')
libmlx=$(shasum -a 256 "$RES/lib/libmlx.dylib" | awk '{print $1}')
decoder_model=$(shasum -a 256 "$RES/AquaTypeDecoder/model.safetensors" | awk '{print $1}') bytes=$(stat -f%z "$RES/AquaTypeDecoder/model.safetensors")
decoder_tables=$(shasum -a 256 "$RES/AquaTypeDecoder/tables.json" | awk '{print $1}')
char_to_keys=$(shasum -a 256 "$RES/AquaTypeData/char_to_keys.json" | awk '{print $1}')
bundle_bytes=$(du -sk "$APP" | awk '{print $1 * 1024}')
EOF
# the bundle has done its job once both packages are built; a loose .app beside them only invites someone to put
# it in place by hand, where nothing registers its input source and it looks installed but is not
rm -rf "$OUT/AquaType.app"
ZIP="$DIST/$NAME.zip"; rm -f "$ZIP"; (cd "$DIST" && ditto -c -k --norsrc --keepParent "$NAME" "$ZIP")
log "built $OUT"; log "zip $ZIP ($(du -h "$ZIP" | awk '{print $1}'))"; log "inventory $DIST/$NAME-inventory.txt"
