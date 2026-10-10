#!/usr/bin/env bash
# Installer and uninstaller packages for AquaType.
#
# What macOS wants from a third-party keyboard input method (Apple QA1810): put the .app in /Library/Input Methods,
# and the person adds it in Settings > Keyboard > Input Sources.  The installer does not enable it, select it, or touch
# the input source preference lists.  Upstream Squirrel ships the same way.
#
# macOS builds the input source list at login, so a new input method appears only after the person logs out and back
# in.  The installer says so on its last page and does not force it: `postinstall-action="logout"` would turn the last
# button into "Log Out", and it has no setting between none and forcing.  A distribution package's conclusion pane
# carries the text instead, above an ordinary Close button.
#
# Not done here, for these reasons:
#   * TISEnableInputSource / TISSelectInputSource: on macOS 26 they return noErr and change nothing.
#   * Writing com.apple.inputsources: it is protected, and rows written there by other means are left behind where only
#     System Settings removes them - one claiming AquaType was enabled hides it from the "+" sheet.
#   * Writing com.apple.HIToolbox: it is writable, but entries written there are duplicated in the input menu.
#
# The postinstall does only what a payload cannot: set up the per-user directory and build the Rime schema in it.
#
#   packaging/build_pkg.sh --bundle dist/<build>/AquaType.app
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
STAMP="$(date +%Y%m%d_%H%M%S)"
BUNDLE=""
while [[ $# -gt 0 ]]; do case "$1" in --bundle) BUNDLE="$2"; shift 2;; *) echo "unknown arg $1" >&2; exit 2;; esac; done
[[ -n "$BUNDLE" && -d "$BUNDLE" ]] || { echo "usage: build_pkg.sh --bundle <AquaType.app>" >&2; exit 2; }
OUT="$(cd "$(dirname "$BUNDLE")" && pwd)"
log() { echo "[build_pkg] $*"; }
need() { [[ -e "$1" ]] || { echo "missing: $1" >&2; exit 1; }; }

IDENTIFIER="com.aquatype.inputmethod.AquaType"
# The file names carry the version and nothing but ASCII: a GitHub release renames an asset whose name has spaces or
# other characters in it, so the names people download would not be the names this build and its README use.
VERSION="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$BUNDLE/Contents/Info.plist")"
INSTALLER="AquaType-$VERSION.pkg"
UNINSTALLER="AquaType-$VERSION-Uninstall.pkg"
INSTALL_LOCATION="/Library/Input Methods"
export COPYFILE_DISABLE=1 COPY_EXTENDED_ATTRIBUTES_DISABLE=1

# An app is signed with "Developer ID Application", an installer package with "Developer ID Installer".  Signing the
# pkg is what stops macOS calling it an unidentified developer; notarisation is separate and is done afterwards by
# packaging/notarize.sh.
PKG_SIGN="${AQUATYPE_INSTALLER_IDENTITY:-}"
[[ -n "$PKG_SIGN" ]] || \
  PKG_SIGN="$(security find-identity -v 2>/dev/null | awk -F'"' '/Developer ID Installer/ {print $2; exit}')"
pkg_sign_args=()
if [[ -n "$PKG_SIGN" ]]; then
  log "installer signing: $PKG_SIGN"
  pkg_sign_args=(--sign "$PKG_SIGN")
else
  log "installer signing: none (no Developer ID Installer identity in the keychain)"
fi

need "$BUNDLE/Contents/MacOS/Squirrel"
need "$BUNDLE/Contents/Resources/AquaTypeEngine"
need "$BUNDLE/Contents/Resources/rime/aquatype_bopomofo.schema.yaml"
VERSION="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$BUNDLE/Contents/Info.plist")"

WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT

# ---- payload: the app, placed by the installer -------------------------------------------------------------------
PKG_ROOT="$WORK/root"; PKG_SCRIPTS="$WORK/scripts"
mkdir -p "$PKG_ROOT" "$PKG_SCRIPTS"
ditto --norsrc --noextattr "$BUNDLE" "$PKG_ROOT/AquaType.app"
/usr/bin/xattr -cr "$PKG_ROOT" >/dev/null 2>&1 || true
find "$PKG_ROOT" -name '._*' -type f -delete
need "$PKG_ROOT/AquaType.app/Contents/MacOS/Squirrel"

# BundleIsRelocatable false: without it the installer will "helpfully" update a copy of AquaType.app it finds somewhere
# else on the disk instead of installing to /Library/Input Methods, which on a machine that has ever built this project
# means a dist/ folder.  BundleOverwriteAction upgrade replaces an existing install in place.
cat > "$WORK/component.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<array>
	<dict>
		<key>RootRelativeBundlePath</key>
		<string>AquaType.app</string>
		<key>BundleHasStrictIdentifier</key>
		<true/>
		<key>BundleIsRelocatable</key>
		<false/>
		<key>BundleIsVersionChecked</key>
		<false/>
		<key>BundleOverwriteAction</key>
		<string>upgrade</string>
		<key>ChildBundles</key>
		<array/>
	</dict>
</array>
</plist>
PLIST


# ---- the scripts the installer runs as root ------------------------------------------------------------------------
# They log to /Library/Logs/AquaType, a folder only root can write: a fixed name in /tmp could be put there beforehand,
# by any account, as a link to a system file that root would then write into.  And root never touches the person's
# home folder: everything there - the per-user Rime tree, the state folder's permissions, removing the personal memory -
# is done as that person, so a link planted in the folder can never turn into a write somewhere only root may write.
#
# AquaType's own processes are matched by their full path, so nothing else whose arguments happen to mention the name
# is stopped.
SCRIPT_COMMON='LOG_DIR="/Library/Logs/AquaType"
/usr/bin/install -d -m 755 -o root -g wheel "$LOG_DIR"
APP="/Library/Input Methods/AquaType.app"
stop_aquatype() {
  /usr/bin/pkill -f "^/Library/Input Methods/AquaType\.app/Contents/Resources/AquaTypeEngine" 2>/dev/null
  /usr/bin/pkill -f "^/Library/Input Methods/AquaType\.app/Contents/MacOS/Squirrel" 2>/dev/null
}
# The person at the screen, and their home folder as the directory service records it (a path may contain spaces).
LOGIN_USER="$(/usr/bin/stat -f%Su /dev/console 2>/dev/null || true)"
case "$LOGIN_USER" in ""|root|_mbsetupuser|loginwindow) LOGIN_USER="" ;; esac
HOME_DIR=""
if [ -n "$LOGIN_USER" ]; then
  HOME_DIR="$(/usr/bin/dscl -plist . -read "/Users/$LOGIN_USER" NFSHomeDirectory 2>/dev/null \
    | /usr/bin/plutil -extract dsAttrTypeStandard:NFSHomeDirectory.0 raw -o - - 2>/dev/null || true)"
fi'

cat > "$PKG_SCRIPTS/preinstall" <<PRE
#!/bin/bash
set -uo pipefail
$SCRIPT_COMMON
exec >>"\$LOG_DIR/install.log" 2>&1
echo "preinstall \$(date)"
stop_aquatype
exit 0
PRE

cat > "$PKG_SCRIPTS/postinstall" <<POST
#!/bin/bash
set -uo pipefail
$SCRIPT_COMMON
exec >>"\$LOG_DIR/install.log" 2>&1
POST
cat >> "$PKG_SCRIPTS/postinstall" <<'POST'
echo "postinstall $(date)"
[ -x "$APP/Contents/MacOS/Squirrel" ] || { echo "the payload did not land at $APP" >&2; exit 1; }

# Without a person logged in at the screen (an installation over ssh or from device management) there is no one to
# set up for: AquaType copies its Rime files into a person's Rime folder and builds them the first time it starts in
# their session (installRimeFiles in SquirrelApplicationDelegate.swift), as it does for every other account.
if [ -z "$LOGIN_USER" ] || [ ! -d "$HOME_DIR" ]; then
  stop_aquatype
  echo "installed; no one is logged in at the screen, so the Rime tree is built at AquaType's first start"
  exit 0
fi

# The per-user Rime tree, built beside the one in use and swapped in whole.  The preinstall stops AquaType, but it is
# the current input source, so macOS starts it again at once - while this script runs - and on its first launch it
# deploys whatever it finds; built in place, both wrote the same files at the same moment and the dictionary build
# failed ("invalid metadata").  Built aside, a running AquaType only ever sees a complete tree.  A tree left by a
# previous version builds stale artefacts over the new schema, so it is replaced rather than merged.  The build is
# checked: one that silently kept an earlier version's schema runs librime parts this version dropped.
/usr/bin/sudo -u "$LOGIN_USER" -H /bin/bash -s -- "$APP" "$HOME_DIR/.aquatype" <<'AS_THE_PERSON'
set -uo pipefail
APP="$1"; STATE_DIR="$2"; RIME_DIR="$STATE_DIR/rime"; NEW_DIR="$STATE_DIR/rime.installing"
umask 077
# A tree that is not finished is not left behind, on a disk that may already be short of room.  After the swap it is
# gone already, and this does nothing.
trap '/bin/rm -rf "$NEW_DIR"' EXIT
/bin/mkdir -p "$STATE_DIR/logs" "$STATE_DIR/run" || exit 1
# Only its owner can read the folder and the personal memory in it, also when an earlier version left them readable.
/bin/chmod 700 "$STATE_DIR"
/bin/chmod 600 "$STATE_DIR"/personal_memory.* 2>/dev/null
/bin/rm -rf "$NEW_DIR"
/usr/bin/ditto --norsrc --noextattr "$APP/Contents/Resources/rime" "$NEW_DIR" || exit 1
# The build this tree comes from, which AquaType compares with its own at start (installRimeFiles): the same build
# means the tree is left as it is, so the person at the screen does not get it copied and built a second time.  Written
# before the build, not after: a file added after it would make the folder newer than the build, and librime would
# build it again at the first start.
printf '%s' "$(/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' "$APP/Contents/Info.plist")" > "$NEW_DIR/.aquatype-build"
/bin/rm -rf "$NEW_DIR/build"
"$APP/Contents/MacOS/Squirrel" --build "$NEW_DIR" "$APP/Contents/SharedSupport" "$NEW_DIR/build" \
  >>"$STATE_DIR/logs/rime_build.log" 2>&1
BUILT="$NEW_DIR/build/aquatype_bopomofo.schema.yaml"
if [ ! -f "$BUILT" ] || [ ! -f "$NEW_DIR/build/aquatype_bopomofo.prism.bin" ]; then
  echo "Rime build failed: $NEW_DIR/build is incomplete" >&2; exit 1
fi
# The same list build_app.sh checks: parts every earlier build has and this version's does not.
/usr/bin/grep -q "delimiter:" "$BUILT" || { echo "Rime build has no speller delimiter" >&2; exit 1; }
for stale in '- punctuator' '- recognizer' '- simplifier' 'full_shape'; do
  if /usr/bin/grep -qF -- "$stale" "$BUILT"; then echo "Rime build is stale: it still has $stale" >&2; exit 1; fi
done
/usr/bin/pkill -f "^/Library/Input Methods/AquaType\.app/Contents/MacOS/Squirrel" 2>/dev/null
/bin/rm -rf "$RIME_DIR.old"
if [ -e "$RIME_DIR" ]; then /bin/mv "$RIME_DIR" "$RIME_DIR.old" || exit 1; fi
# macOS can start AquaType again in the moment between the two moves, and it makes an empty Rime folder at start; the
# tree would then be moved into that folder instead of becoming it.
/bin/rm -rf "$RIME_DIR"
/bin/mv "$NEW_DIR" "$RIME_DIR" || exit 1
/bin/rm -rf "$RIME_DIR.old"
exit 0
AS_THE_PERSON
# Not a failed installation: AquaType is in place, and it sets the tree up itself at its first start, as it does for
# every account the installer does not.
[ $? -eq 0 ] || echo "setting up $LOGIN_USER's Rime tree failed; AquaType sets it up at its first start"

# Then AquaType is restarted, so that the copy macOS started during the install reads the new tree.
stop_aquatype

# Nothing here registers, enables or selects the input source, and nothing here writes an input source preference
# list.  macOS rebuilds that list at login; the installer's conclusion pane says so.  After logging back in they add
# AquaType in Settings > Keyboard > Input Sources, the same as every other third-party input method.
echo "installed; the person logs out, logs in, and adds AquaType in Settings"
exit 0
POST

chmod +x "$PKG_SCRIPTS/preinstall" "$PKG_SCRIPTS/postinstall"

# ---- the pages the installer shows ------------------------------------------------------------------------------------
# Installer.app writes the frame of each page itself, and these pages only fill it: the welcome page is already headed
# "歡迎使用「<title>」安裝程式", the last one "已成功安裝。", and the buttons are its own.  So no page repeats a heading.
#
# Installer.app draws the text in its own colour, black or white with the window's appearance, so the pages set none.
# Each page fits its box without scrolling where it can, and each paragraph is one line in the source: a line break
# inside Chinese text is drawn as a space.
#
# The licences are a Read Me page, not a licence page.  A licence page makes the person press 同意 before installing,
# and nothing here asks for that - GPL-3.0 says outright that running the program does not require accepting it.
RES_DIR="$WORK/resources"; mkdir -p "$RES_DIR"
PAGE_HEAD='<!DOCTYPE html><html><head><meta charset="utf-8">
<style>
 body { font: 13px -apple-system, "Helvetica Neue", sans-serif; margin: 0; }
 p { margin: 0 0 10px; line-height: 1.5; }
 h3 { font-size: 13px; margin: 14px 0 4px; }
 ul { margin: 0 0 10px 0; padding-left: 20px; line-height: 1.5; }
</style></head><body>'

cat > "$RES_DIR/welcome.html" <<HTML
$PAGE_HEAD
<p>AquaType 是適用於 Mac 的注音輸入法，使用標準注音鍵盤（大千式）。輸入一整句注音後，AquaType 會以在這台 Mac 上執行的語言模型轉換成中文；按錯鍵時，候選中也會列出修正後的句子。</p>
<h3>系統需求</h3>
<ul>
  <li>配備 Apple 晶片的 Mac。建議使用 M3 或後續晶片；在 M1、M2 上，候選會出現得比較慢。</li>
  <li>macOS Sonoma 14 或以上版本。</li>
</ul>
<h3>隱私權</h3>
<p>AquaType 完全離線，只在這台 Mac 上執行，不會連線到網際網路，也不會傳送任何資料。你送出的字詞會連同當時按下的按鍵、次數和時間記錄在這台 Mac 上，稱為「個人記憶」，用來調整候選的順序；游標前的文字只在轉換時作為上下文使用，不會被記錄。移除 AquaType 時，目前使用者帳號的個人記憶會一併刪除。</p>
<p>這是 AquaType ${VERSION} 公開測試版。安裝完成後，你需要登出再重新登入，才能開始使用。</p>
</body></html>
HTML
cat > "$RES_DIR/readme.html" <<HTML
$PAGE_HEAD
<h3 style="margin-top:0">安裝位置</h3>
<p>AquaType 會安裝在「/資源庫/Input Methods」，這台 Mac 上的所有使用者都可以使用。每位使用者的設定和個人記憶，存放在各自個人專屬檔案夾中的「.aquatype」檔案夾。</p>
<h3>移除 AquaType</h3>
<p>打開與這個安裝程式一起提供的「${UNINSTALLER}」。它會刪除 AquaType，以及目前使用者帳號中的設定、記錄檔和個人記憶。</p>
<h3>授權條款</h3>
<p>AquaType 由下列元件組成，各元件依其授權條款提供：</p>
<ul>
  <li><b>輸入法應用程式</b>：衍生自鼠鬚管（Squirrel，著作權屬於佛振及鼠鬚管貢獻者），依 GNU 通用公共授權條款第 3 版（GPL-3.0）提供。每個版本都會公開完整原始碼。</li>
  <li><b>引擎與解碼器</b>：依 Apache License 2.0 提供。</li>
  <li><b>語言模型</b>：依創用 CC 姓名標示-非商業性 4.0 國際（CC BY-NC 4.0）提供，僅限非商業用途。</li>
  <li><b>MLX</b>（Apple Inc.）：依 MIT License 提供。</li>
  <li><b>詞庫資料</b>：取自小麥注音、教育部辭典（萌典）、sc-dictionary、Rime 地球拼音、pypinyin、jieba、CC-CEDICT、維基百科、ICU 等來源，依各來源的授權條款使用。</li>
  <li><b>Rime 基本設定、注音拼寫規則與詞頻表</b>：依 GNU 較寬鬆公共授權條款第 3 版（LGPL-3.0）提供。</li>
</ul>
<p>各授權條款的全文和完整出處說明，位於安裝後的「AquaType.app/Contents/Resources/licenses」。</p>
<h3>免責聲明</h3>
<p>本軟體依「現狀」提供，不附帶任何明示或默示的擔保，包括但不限於適售性及特定用途適用性的擔保。</p>
</body></html>
HTML

# ---- the text the installer ends on -------------------------------------------------------------------------------
# A <conclusion> resource is shown on the installer's last page, under "已成功安裝。", with a Close button.
# This is where the logout goes: stated, with the reason, and not imposed.  The Settings path is spelled the way
# System Settings labels it in Traditional Chinese on macOS 26 (鍵盤 > 文字輸入 > 輸入方式 > 編輯⋯).
cat > "$RES_DIR/conclusion.html" <<HTML
$PAGE_HEAD
<p>完成下列兩個步驟後，就可以開始使用 AquaType：</p>
<p><b>1. 登出再重新登入。</b>選擇「蘋果」選單 >「登出」，然後重新登入。macOS 會在登入時載入新安裝的輸入法。你可以稍後再登出。</p>
<p><b>2. 加入 AquaType。</b>選擇「蘋果」選單 >「系統設定」，按一下側邊欄中的「鍵盤」，前往「文字輸入」，然後按一下「編輯」。按一下加入按鈕「+」，選取「繁體中文」，再選取右側列表中的「AquaType」，然後按一下「加入」。</p>
<p>加入後，你可以從選單列的輸入法選單切換到 AquaType。</p>
</body></html>
HTML

cat > "$WORK/Distribution.xml" <<XML
<?xml version="1.0" encoding="utf-8" standalone="no"?>
<installer-gui-script minSpecVersion="2">
    <title>AquaType $VERSION</title>
    <organization>com.aquatype</organization>
    <options customize="never" require-scripts="true" hostArchitectures="arm64"/>
    <domains enable_localSystem="true" enable_anywhere="false" enable_currentUserHome="false"/>
    <allowed-os-versions>
        <os-version min="14.0"/>
    </allowed-os-versions>
    <welcome file="welcome.html" mime-type="text/html"/>
    <readme file="readme.html" mime-type="text/html"/>
    <conclusion file="conclusion.html" mime-type="text/html"/>
    <choices-outline>
        <line choice="default"/>
    </choices-outline>
    <choice id="default" visible="false">
        <pkg-ref id="$IDENTIFIER"/>
    </choice>
    <pkg-ref id="$IDENTIFIER" version="$STAMP" onConclusion="none">AquaType-component.pkg</pkg-ref>
</installer-gui-script>
XML

log "building $INSTALLER"
pkgbuild --root "$PKG_ROOT" \
         --component-plist "$WORK/component.plist" \
         --identifier "$IDENTIFIER" \
         --version "$STAMP" \
         --install-location "$INSTALL_LOCATION" \
         --scripts "$PKG_SCRIPTS" \
         "$WORK/AquaType-component.pkg" >/dev/null
productbuild --distribution "$WORK/Distribution.xml" \
             --resources "$RES_DIR" \
             --package-path "$WORK" \
             "${pkg_sign_args[@]}" "$OUT/$INSTALLER" >/dev/null

# ---- uninstaller ---------------------------------------------------------------------------------------------------
# Delete what was installed, forget the receipt, and ask for a logout so the input menu is rebuilt without it.  No
# dialogs, no preference editing, no requirement to visit Settings first.
UN_SCRIPTS="$WORK/unscripts"; mkdir -p "$UN_SCRIPTS"
cat > "$UN_SCRIPTS/postinstall" <<UN
#!/bin/bash
set -uo pipefail
$SCRIPT_COMMON
exec >>"\$LOG_DIR/uninstall.log" 2>&1
UN
cat >> "$UN_SCRIPTS/postinstall" <<'UN'
echo "uninstall $(date)"
LSREGISTER="/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister"

stop_aquatype
if [ -e "$APP" ]; then
  "$LSREGISTER" -u "$APP" 2>/dev/null
  /bin/rm -rf "$APP"
fi
# macOS starts an active input method again as soon as it ends, so one may have started between the stop above and
# the removal; with the app gone, this stop is the last.  Before the home folder, which a running copy would write to.
stop_aquatype

# Everything AquaType writes in the person's home folder, the personal memory included - removing means removing -
# and a copy an early test build put in their own Input Methods folder.  Removed as that person, not as root.
if [ -n "$LOGIN_USER" ] && [ -d "$HOME_DIR" ]; then
  /usr/bin/sudo -u "$LOGIN_USER" -H /bin/bash -s -- "$HOME_DIR" <<'AS_THE_PERSON'
HOME_DIR="$1"; BID="com.aquatype.inputmethod.AquaType"
/bin/rm -rf "$HOME_DIR/.aquatype" \
            "$HOME_DIR/Library/Input Methods/AquaType.app" \
            "$HOME_DIR/Library/Caches/$BID" \
            "$HOME_DIR/Library/HTTPStorages/$BID" \
            "$HOME_DIR/Library/WebKit/$BID" \
            "$HOME_DIR/Library/Application Support/AquaType" \
            "$HOME_DIR/Library/Preferences/$BID.plist" \
            "$HOME_DIR/Library/Preferences/$BID.plist.lockfile" \
            "$HOME_DIR/Library/Saved Application State/$BID.savedState"
AS_THE_PERSON
fi

/usr/sbin/pkgutil --pkgs | /usr/bin/grep -i '^com\.aquatype\.' | while IFS= read -r receipt; do
  /usr/sbin/pkgutil --forget "$receipt" 2>/dev/null
done
# This uninstaller's own receipt is registered after the script returns, so it is forgotten a moment later.
( /bin/sleep 8
  /usr/sbin/pkgutil --pkgs | /usr/bin/grep -i '^com\.aquatype\.' | while IFS= read -r receipt; do
    /usr/sbin/pkgutil --forget "$receipt" 2>/dev/null
  done
) </dev/null >/dev/null 2>&1 &

echo "removed; the input menu is rebuilt at the next login"
# The installer's and this script's logs go last, once everything else has been removed: an uninstall that stops
# early keeps them, to show where it stopped.
/bin/rm -rf "$LOG_DIR"
exit 0
UN
chmod +x "$UN_SCRIPTS/postinstall"

# Installer.app has no words for removing: its button says 安裝 and its last page 已成功安裝。  So the first page says,
# before anything is deleted, that this removes AquaType, what goes with it, and that the personal memory cannot be
# brought back.
UN_RES="$WORK/unresources"; mkdir -p "$UN_RES"
cat > "$UN_RES/welcome.html" <<HTML
$PAGE_HEAD
<p>這個程式會從這台 Mac 移除 AquaType。</p>
<h3>會移除的項目</h3>
<ul>
  <li>AquaType 輸入法</li>
  <li>目前使用者帳號中的 AquaType 設定、記錄檔和個人記憶</li>
</ul>
<p>其他輸入法和系統設定不會受到影響。</p>
<p>移除前，請先從選單列的輸入法選單切換到其他輸入法（例如 ABC 或注音）。</p>
<p><b>個人記憶刪除後無法復原。</b></p>
<p>若要移除 AquaType，請按一下「繼續」，然後按一下「安裝」。macOS 安裝程式的按鈕固定顯示為「安裝」，在這裡代表執行移除。</p>
</body></html>
HTML
cat > "$UN_RES/conclusion.html" <<HTML
$PAGE_HEAD
<p><b>AquaType 已從這台 Mac 移除。</b></p>
<p>AquaType、它的安裝記錄，以及目前使用者帳號中的 AquaType 設定、記錄檔和個人記憶都已刪除。其他輸入法不受影響。</p>
<p>請登出再重新登入，AquaType 就會從輸入法選單中消失。你可以稍後再登出。</p>
</body></html>
HTML

cat > "$WORK/DistributionUninstall.xml" <<XML
<?xml version="1.0" encoding="utf-8" standalone="no"?>
<installer-gui-script minSpecVersion="2">
    <title>移除 AquaType</title>
    <organization>com.aquatype</organization>
    <options customize="never" require-scripts="true" hostArchitectures="arm64"/>
    <domains enable_localSystem="true" enable_anywhere="false" enable_currentUserHome="false"/>
    <welcome file="welcome.html" mime-type="text/html"/>
    <conclusion file="conclusion.html" mime-type="text/html"/>
    <choices-outline>
        <line choice="default"/>
    </choices-outline>
    <choice id="default" visible="false">
        <pkg-ref id="$IDENTIFIER.uninstall"/>
    </choice>
    <pkg-ref id="$IDENTIFIER.uninstall" version="$STAMP" onConclusion="none">AquaType-uninstall-component.pkg</pkg-ref>
</installer-gui-script>
XML

log "building the uninstaller"
pkgbuild --nopayload --scripts "$UN_SCRIPTS" \
         --identifier "$IDENTIFIER.uninstall" \
         --version "$STAMP" \
         "$WORK/AquaType-uninstall-component.pkg" >/dev/null
productbuild --distribution "$WORK/DistributionUninstall.xml" \
             --resources "$UN_RES" \
             --package-path "$WORK" \
             "${pkg_sign_args[@]}" "$OUT/$UNINSTALLER" >/dev/null

# ---- check what was built ------------------------------------------------------------------------------------------
# Both halves of the logout behaviour are verified rather than assumed, because each one fails silently:
#   * no conclusion pane -> the installer finishes without ever mentioning the logout;
#   * an onConclusion that is not "none" -> the installer forces the logout.  A stray RequireLogout looks identical in
#     the build log and only shows up in front of a person.
for p in "$OUT/$INSTALLER" "$OUT/$UNINSTALLER"; do
  d="$WORK/expand-$(basename "$p" .pkg)"
  pkgutil --expand "$p" "$d" >/dev/null
  grep -q 'conclusion.html' "$d/Distribution" \
    || { echo "refusing: $p has no conclusion pane, so it never mentions the logout" >&2; exit 1; }
  [[ -s "$d/Resources/conclusion.html" ]] \
    || { echo "refusing: $p declares a conclusion pane but does not carry it" >&2; exit 1; }
  grep -q '登出' "$d/Resources/conclusion.html" \
    || { echo "refusing: $p's conclusion pane does not mention logging out" >&2; exit 1; }
  if grep -Eq 'onConclusion="(RequireLogout|RequireRestart|RequireShutdown|RecommendRestart)"' "$d/Distribution" \
     || grep -rqs 'postinstall-action="\(logout\|restart\|shutdown\)"' "$d"; then
    echo "refusing: $p forces a logout or restart; it should only say one is needed" >&2; exit 1
  fi
  if [[ "$p" == "$OUT/$UNINSTALLER" ]]; then
    # The uninstaller says what it deletes before it deletes it - the personal memory cannot be brought back.
    grep -q 'welcome.html' "$d/Distribution" && grep -q '無法復原' "$d/Resources/welcome.html" \
      || { echo "refusing: $p deletes the personal memory without saying so first" >&2; exit 1; }
  fi
  if [[ "$p" == "$OUT/$INSTALLER" ]]; then
    # Nothing here asks the person to accept a licence before installing (GPL-3.0 does not require it to run).
    ! grep -q '<license' "$d/Distribution" \
      || { echo "refusing: $p asks for a licence to be accepted" >&2; exit 1; }
    # An Intel Mac or a macOS older than 14 gets the installer's own "cannot be installed on this computer" instead
    # of an install that cannot run.  Both limits are checked in the built file, since either one missing fails
    # silently - it only shows up as an install on a machine that should have been turned away.
    grep -q 'hostArchitectures="arm64"' "$d/Distribution" && grep -q '<os-version min="14.0"/>' "$d/Distribution" \
      || { echo "refusing: $p does not restrict itself to Apple silicon on macOS 14 or later" >&2; exit 1; }
  fi
  # The scripts work on the startup disk only (/Library/Input Methods), so the installer offers no other disk: chosen,
  # one would fail a fresh install and report an upgrade done that never happened.
  grep -q '<domains enable_localSystem="true" enable_anywhere="false" enable_currentUserHome="false"/>' "$d/Distribution" \
    || { echo "refusing: $p lets the person choose a disk other than the startup disk" >&2; exit 1; }
  sig="$(pkgutil --check-signature "$p" 2>/dev/null | awk -F': *' '/Status:/ {print $2; exit}')"
  log "pkg $p  [${sig:-unsigned}]  [says to log out, does not force it]"
done

# The installer must place the app at /Library/Input Methods/AquaType.app and nowhere else.
pkgutil --payload-files "$WORK/AquaType-component.pkg" | grep -q '^./AquaType.app/Contents/MacOS/Squirrel$' \
  || { echo "refusing: the payload does not contain AquaType.app/Contents/MacOS/Squirrel" >&2; exit 1; }
log "payload verified: AquaType.app installs to $INSTALL_LOCATION"
