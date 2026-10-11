# Assembles the install folder (dist\AquaType-win) from the build and the restored payload:
#   AquaTypeTSF.dll  AquaTypeEngine.exe  aquadecoder.dll  data\decoder\*  data\dict_store\*  data\char_to_keys.json
#   install.ps1  uninstall.ps1  licenses\*
#   powershell -File windows\install\stage.ps1
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path "$PSScriptRoot\..\..").Path
$build = "$repo\windows\build"
$out = "$repo\dist\AquaType-win"
if (Test-Path $out) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory -Force "$out\data\decoder", "$out\data\dict_store", "$out\licenses" | Out-Null
Copy-Item "$build\AquaTypeTSF.dll", "$build\AquaTypeEngine.exe", "$build\aquadecoder.dll" $out
# for 32-bit apps: cmake -S windows -B windows\build32 -G Ninja -DCMAKE_BUILD_TYPE=Release -DAQ_TIP_ONLY=ON (vcvars32)
New-Item -ItemType Directory -Force "$out\x86" | Out-Null
Copy-Item "$repo\windows\build32\AquaTypeTSF.dll" "$out\x86"
$dec = "$repo\packaging\payload\decoder"
Copy-Item "$dec\model.json", "$dec\model.safetensors", "$dec\norm_rank.bin", "$dec\tables.json", "$dec\tables.safetensors" "$out\data\decoder"
$ds = "$repo\packaging\payload\dict_store"
Copy-Item "$ds\dictionary_fallback.v1.aqds", "$ds\dictionary_fallback.v1.aqds.manifest.json" "$out\data\dict_store"
Copy-Item "$repo\data\char_to_keys.json" "$out\data"
Copy-Item "$PSScriptRoot\install.ps1", "$PSScriptRoot\uninstall.ps1" $out
Copy-Item "$repo\LICENSE", "$repo\NOTICE.txt" "$out\licenses"
Copy-Item "$repo\packaging\legal\*" "$out\licenses"
$size = (Get-ChildItem -Recurse $out | Measure-Object Length -Sum).Sum / 1MB
"staged $out ({0:N0} MB)" -f $size
