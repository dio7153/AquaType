# Installs AquaType for the current user's machine.  Run from the staged folder, as administrator:
#   powershell -ExecutionPolicy Bypass -File install.ps1
# 1. copies this folder to C:\Program Files\AquaType (files still loaded somewhere are renamed aside first)
# 2. registers the text service, 64-bit and 32-bit (regsvr32)
# 3. adds "AquaType 注音" to this user's keyboards (zh-TW)
# 4. starts the engine at logon (HKCU Run); until then the first app that uses AquaType starts it
param([string]$Target = "$env:ProgramFiles\AquaType")
$ErrorActionPreference = 'Stop'
$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw "run this as administrator" }
$src = $PSScriptRoot
$clsid = '{87FA8DDA-2BA4-476C-8510-93DCD553AD2B}'; $prof = '{3A6B588B-D85E-4EB1-A28D-38F3F7BEBE92}'
$stamp = Get-Date -Format yyyyMMddHHmmss

Get-Process AquaTypeEngine -ErrorAction SilentlyContinue | Stop-Process -Force
New-Item -ItemType Directory -Force $Target, "$Target\x86" | Out-Null
# Loaded files cannot be overwritten, but they can be renamed; old renamed copies go once nothing holds them.
foreach ($f in "$Target\AquaTypeTSF.dll", "$Target\x86\AquaTypeTSF.dll", "$Target\AquaTypeEngine.exe", "$Target\aquadecoder.dll", "$Target\aquadecoder_avx2.dll", "$Target\aquadecoder_sse.dll") {
    if (Test-Path $f) { try { Remove-Item $f -Force } catch { Rename-Item $f ((Split-Path $f -Leaf) + ".old-$stamp") } }
}
Get-ChildItem $Target -Recurse -Filter '*.old-*' -ErrorAction SilentlyContinue | ForEach-Object { try { Remove-Item $_.FullName -Force } catch {} }
Copy-Item -Recurse -Force (Join-Path $src '*') $Target

$dll = "$Target\AquaTypeTSF.dll"; $dll32 = "$Target\x86\AquaTypeTSF.dll"
$p = Start-Process -FilePath "$env:WINDIR\System32\regsvr32.exe" -ArgumentList "/s `"$dll`"" -Wait -PassThru -WindowStyle Hidden
if ($p.ExitCode -ne 0) { throw "regsvr32 failed ($($p.ExitCode))" }
if (Test-Path $dll32) {                                 # the 32-bit copy, for 32-bit apps
    $p = Start-Process -FilePath "$env:WINDIR\SysWOW64\regsvr32.exe" -ArgumentList "/s `"$dll32`"" -Wait -PassThru -WindowStyle Hidden
    if ($p.ExitCode -ne 0) { Write-Warning "32-bit registration failed ($($p.ExitCode)); 32-bit apps will not have AquaType" }
}

Add-Type -Namespace AquaType -Name Input -MemberDefinition '[DllImport("input.dll", CharSet = CharSet.Unicode)] public static extern bool InstallLayoutOrTip(string psz, uint flags);'
if (-not [AquaType.Input]::InstallLayoutOrTip("0404:$clsid$prof", 0)) { Write-Warning "could not add the keyboard; add it in Settings > Time & language > Language > 中文(台灣) > Keyboards" }

$engine = "$Target\AquaTypeEngine.exe"; $data = "$Target\data"; $log = "$env:LOCALAPPDATA\AquaType\engine.log"
New-Item -ItemType Directory -Force (Split-Path $log) | Out-Null
$cmd = "`"$engine`" --command serve-pipe --decoder-dir `"$data\decoder`" --char-to-keys `"$data\char_to_keys.json`" --fallback-aqds `"$data\dict_store\dictionary_fallback.v1.aqds`" --fallback-manifest `"$data\dict_store\dictionary_fallback.v1.aqds.manifest.json`" --log `"$log`""
Set-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' -Name 'AquaTypeEngine' -Value $cmd
# Not started from here: this script runs elevated, and the engine must not.  The first app that switches to AquaType
# starts it (at normal integrity), and so does every logon from now on.
"installed to $Target; switch input methods with Win+Space and choose AquaType 注音"
