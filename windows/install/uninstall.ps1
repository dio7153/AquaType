# Removes AquaType, as administrator:
#   powershell -ExecutionPolicy Bypass -File "C:\Program Files\AquaType\uninstall.ps1"
# The personal memory (%USERPROFILE%\.aquatype) is kept; delete that folder too to forget everything typed.
param([string]$Target = "$env:ProgramFiles\AquaType")
$ErrorActionPreference = 'Continue'
$clsid = '{87FA8DDA-2BA4-476C-8510-93DCD553AD2B}'; $prof = '{3A6B588B-D85E-4EB1-A28D-38F3F7BEBE92}'
Add-Type -Namespace AquaType -Name Input -MemberDefinition '[DllImport("input.dll", CharSet = CharSet.Unicode)] public static extern bool InstallLayoutOrTip(string psz, uint flags);'
[void][AquaType.Input]::InstallLayoutOrTip("0404:$clsid$prof", 1)          # ILOT_UNINSTALL
Remove-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' -Name 'AquaTypeEngine' -ErrorAction SilentlyContinue
Remove-Item "$env:ProgramData\Microsoft\Windows\Start Menu\Programs\AquaType 設定.lnk" -Force -ErrorAction SilentlyContinue
Get-Process AquaTypeSettings -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$Target\*" } | Stop-Process -Force
Get-Process AquaTypeEngine -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$Target\*" } | Stop-Process -Force   # only the installed one
if (Test-Path "$Target\x86\AquaTypeTSF.dll") { Start-Process -FilePath "$env:WINDIR\SysWOW64\regsvr32.exe" -ArgumentList "/u /s `"$Target\x86\AquaTypeTSF.dll`"" -Wait -WindowStyle Hidden }
if (Test-Path "$Target\AquaTypeTSF.dll") { Start-Process -FilePath "$env:WINDIR\System32\regsvr32.exe" -ArgumentList "/u /s `"$Target\AquaTypeTSF.dll`"" -Wait -WindowStyle Hidden }
Remove-Item -Recurse -Force $Target -ErrorAction SilentlyContinue
if (Test-Path $Target) { "some files are still in use; sign out and delete $Target" } else { "removed" }
