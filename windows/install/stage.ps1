# Assembles the install folder (dist\AquaType-win) from the build and the restored payload:
#   AquaTypeTSF.dll  AquaTypeEngine.exe  AquaTypeSettings.exe  aquadecoder.dll (+ _avx2 / _sse, the one it loads)  data\decoder\*  data\dict_store\*  data\char_to_keys.json  data\terra_pinyin.dict.yaml
#   install.ps1  uninstall.ps1  licenses\*
#   powershell -File windows\install\stage.ps1
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path "$PSScriptRoot\..\..").Path
$build = "$repo\windows\build"
$out = "$repo\dist\AquaType-win"
if (Test-Path $out) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory -Force "$out\data\decoder", "$out\data\dict_store", "$out\licenses" | Out-Null
Copy-Item "$build\AquaTypeTSF.dll", "$build\AquaTypeEngine.exe", "$build\aquadecoder.dll", "$build\aquadecoder_avx2.dll", "$build\aquadecoder_sse.dll", "$build\AquaTypeSettings.exe" $out
# for 32-bit apps: cmake -S windows -B windows\build32 -G Ninja -DCMAKE_BUILD_TYPE=Release -DAQ_TIP_ONLY=ON (vcvars32)
New-Item -ItemType Directory -Force "$out\x86" | Out-Null
Copy-Item "$repo\windows\build32\AquaTypeTSF.dll" "$out\x86"
$dec = "$repo\packaging\payload\decoder"
Copy-Item "$dec\model.json", "$dec\model.safetensors", "$dec\norm_rank.bin", "$dec\tables.json", "$dec\tables.safetensors" "$out\data\decoder"
$ds = "$repo\packaging\payload\dict_store"
Copy-Item "$ds\dictionary_fallback.v1.aqds", "$ds\dictionary_fallback.v1.aqds.manifest.json" "$out\data\dict_store"
Copy-Item "$repo\data\char_to_keys.json" "$out\data"
Copy-Item "$repo\packaging\payload\rime\terra_pinyin.dict.yaml" "$out\data"   # a character's other readings: the engine reads it beside char_to_keys.json
Copy-Item "$PSScriptRoot\install.ps1", "$PSScriptRoot\uninstall.ps1" $out
Copy-Item "$repo\LICENSE", "$repo\NOTICE.txt" "$out\licenses"
Copy-Item "$repo\packaging\legal\*" "$out\licenses"
Copy-Item "$repo\third_party\squirrel\LICENSE.txt" "$out\licenses\LICENSE-GPL-3.0.txt"     # the text service and the settings window
$size = (Get-ChildItem -Recurse $out | Measure-Object Length -Sum).Sum / 1MB
"staged $out ({0:N0} MB)" -f $size
