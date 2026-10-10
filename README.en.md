# AquaType

[中文](README.md) | **English**

AquaType is a Zhuyin input method for Mac. Even if you press the wrong keys, AquaType determines what you meant to type and suggests the right words.

Built on RIME (Squirrel) with a noisy-channel decoding engine that runs entirely on device and works offline.

[![Release](https://img.shields.io/github/v/release/dio7153/AquaType?include_prereleases&label=release)](../../releases)
![macOS 14+](https://img.shields.io/badge/macOS-14%2B-000000?logo=apple&logoColor=white)
![Apple Silicon](https://img.shields.io/badge/Apple%20Silicon-M1%2B-555555)
[![License](https://img.shields.io/badge/license-GPL--3.0%20%2F%20Apache--2.0%20%2F%20CC%20BY--NC%204.0-blue)](NOTICE.txt)
[![DOI](https://zenodo.org/badge/DOI/10.5281/zenodo.23289528.svg)](https://doi.org/10.5281/zenodo.23289528)

<img src="docs/images/demo.gif" width="560" alt="Even with many wrong keys, AquaType suggests the right sentence">

## Features

- **Typo tolerance:** When you press a neighboring key, miss a key, press an extra key, or swap two keys, AquaType works out the words you meant to type.
- **Whole-sentence input:** Type a full sentence in Zhuyin and AquaType suggests the complete sentence. You can also press Tab to choose words one segment at a time.
- **Personal memory:** AquaType remembers the words you choose and suggests them first next time.
- **On-device processing:** Everything happens on your Mac. AquaType never connects to the internet or sends any data.
- **Familiar controls:** Punctuation works the same as in the built-in Zhuyin input method, and you can press Shift to switch between Chinese and English.

## System requirements

- A Mac with Apple silicon. M3 or later is recommended; on M1 and M2, suggestions appear more slowly.
- macOS Sonoma 14 or later.

## Install

1. Download AquaType-0.9.0.pkg from [Releases](../../releases), open it, and follow the onscreen instructions.
2. Choose Apple menu > Log Out, then log back in.
3. Open System Settings > Keyboard > Text Input, then click Edit. Click the Add button (+), select Chinese, Traditional, then click Add.

> [!NOTE]
> If AquaType appears in the list, select it before you click Add. If the list on the right shows only a message that no keyboard layout is available, just click Add.

## Uninstall

1. Switch to another input source from the Input menu, such as ABC.
2. Download AquaType-0.9.0-Uninstall.pkg from [Releases](../../releases), open it, and follow the onscreen instructions.
3. Choose Apple menu > Log Out, then log back in.

> [!WARNING]
> Uninstalling AquaType also deletes your personal memory, and it can't be recovered.

## Privacy

- AquaType never connects to the internet or sends any data.
- The words you enter are stored on your Mac, along with the keys you pressed (including mistyped keys), how often you used them, and when. They're used to adjust the order of suggestions. Up to 4,000 entries are stored; beyond that, the least-used entries are removed first.
- To suggest more fitting words, AquaType reads up to 256 characters before the cursor. This text isn't stored. When Secure Input is on, AquaType doesn't read any text or store any entries.
- To clear your personal memory, delete the files whose names begin with "personal_memory" in the ~/.aquatype folder, then log out and log back in.

## Known limitations

- Only the standard Zhuyin keyboard layout is supported, used together with the ABC or U.S. keyboard layout. With Dvorak, French, or other layouts, Zhuyin keys are mapped incorrectly.
- You can't choose suggestions with number keys. Use the arrow keys, or click a suggestion.
- To enter Zhuyin symbols or special characters, use the Emoji & Symbols viewer (press Fn-E or Globe-E).
- While you're composing, you can't move the cursor into the middle to change a single word.
- Each entry is limited to about 60 characters. When you reach the limit, new Zhuyin keys aren't entered and an alert sounds.
- The longer the sentence, or the more keys you mistype, the more likely it is that the correct result isn't among the suggestions. Press Tab to choose words one segment at a time, or press Delete to make changes.
- VoiceOver isn't supported.

## Report an issue

Report issues in [Issues](../../issues), and include your macOS version, your Mac model, and the app where the issue occurred. To report a security issue privately, see [SECURITY.md](SECURITY.md).

AquaType is maintained by one person, so replies may take a few days. If you'd like to change the code, please open an issue to discuss it first.

## License and acknowledgments

- The AquaType input method is modified from [Squirrel](https://github.com/rime/squirrel) and released under the GNU General Public License v3.0 (GPL-3.0).
- The AquaType engine and decoder are released under the Apache License 2.0.
- The models are released under the Creative Commons Attribution-NonCommercial 4.0 International license (CC BY-NC 4.0) and are for non-commercial use only. They were trained from scratch, without any pretrained model; for the training data, see [MODEL.en.md](MODEL.en.md).
- Dictionary data comes from several open sources, each under its own license. For the full license texts and data sources, see [NOTICE.txt](NOTICE.txt).

AquaType was developed with the following projects, data, and tools:

- The [RIME](https://rime.im) input method engine and Squirrel, by Lotem (佛振) and contributors
- Zhuyin data from [McBopomofo](https://github.com/openvanilla/McBopomofo)
- The Ministry of Education's Revised Mandarin Chinese Dictionary (重編國語辭典修訂本)
- Apple's [MLX](https://github.com/ml-explore/mlx)
- Word data from [sc-dictionary](https://github.com/samejack/sc-dictionary), [rime-terra-pinyin](https://github.com/rime/rime-terra-pinyin), [pypinyin](https://github.com/mozillazg/python-pinyin), and others
- [Claude Code](https://github.com/anthropics/claude-code) (Anthropic) and [Codex](https://github.com/openai/codex) (OpenAI), for help writing code and documentation
