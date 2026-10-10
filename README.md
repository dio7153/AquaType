# AquaType

**中文** | [English](README.en.md)

AquaType 是適用於 Mac 的注音輸入法。即使按錯鍵，AquaType 也能判斷你想輸入的內容，列出正確的字詞。

以 RIME（鼠鬚管）為基礎，雜訊通道解碼引擎完全在裝置端執行，可離線運作。

AquaType is a Zhuyin input method for Mac. Even if you press the wrong keys, AquaType determines what you meant to type and suggests the right words.<br>
Built on RIME (Squirrel) with a noisy-channel decoding engine that runs entirely on device and works offline.

[![Release](https://img.shields.io/github/v/release/dio7153/AquaType?include_prereleases&label=release)](../../releases)
![macOS 14+](https://img.shields.io/badge/macOS-14%2B-000000?logo=apple&logoColor=white)
![Apple Silicon](https://img.shields.io/badge/Apple%20Silicon-M1%2B-555555)
[![License](https://img.shields.io/badge/license-GPL--3.0%20%2F%20Apache--2.0%20%2F%20CC%20BY--NC%204.0-blue)](NOTICE.txt)

<img src="docs/images/demo.gif" width="560" alt="按錯了很多鍵，AquaType 仍然列出正確的句子">

## 特色

- **容錯輸入：** 按到相鄰的鍵、漏按、多按，或前後兩鍵按反時，AquaType 會判斷你原本想輸入的字詞。
- **整句輸入：** 連續輸入整句注音，AquaType 會一次列出完整的句子；也能按 Tab 分段選字。
- **個人記憶：** AquaType 會記住你選過的字詞，下次輸入時優先列出。
- **裝置端處理：** 所有處理都在你的 Mac 上進行，不會連上網路或傳送任何資料。
- **沿用習慣操作：** 標點符號與 macOS 內建注音相同，按 Shift 即可切換中文和英文。

## 系統需求

- 配備 Apple 晶片的 Mac。建議使用 M3 或後續晶片；在 M1、M2 上，候選會出現得比較慢。
- macOS Sonoma 14 或以上版本。

## 安裝

1. 從 [Releases](../../releases) 下載「AquaType-0.9.0.pkg」並打開，依照畫面上的指示完成安裝。
2. 選擇「蘋果」選單 >「登出」，然後重新登入。
3. 打開「系統設定」>「鍵盤」>「文字輸入」，按一下「編輯」。按一下「+」，選取「繁體中文」，然後按一下「加入」。

> [!NOTE]
> 如果列表中出現 AquaType，請先選取它再按一下「加入」；如果右側只顯示「無法使用鍵盤佈局」，直接按一下「加入」即可。

## 移除

1. 從輸入法選單切換到其他輸入法，例如「ABC」。
2. 從 [Releases](../../releases) 下載並打開「AquaType-0.9.0-Uninstall.pkg」，依照畫面上的指示完成移除。
3. 選擇「蘋果」選單 >「登出」，然後重新登入。

> [!WARNING]
> 移除 AquaType 時，個人記憶會一併刪除，且無法復原。

## 隱私權

- AquaType 不會連上網路，也不會傳送任何資料。
- 你送出的字詞會連同按下的按鍵（包括按錯的鍵）、使用次數和時間，儲存在你的 Mac 上，用來調整候選的順序。最多儲存 4,000 筆；超過時，較少使用的記錄會先被移除。
- AquaType 會讀取游標前最多 256 個字元來列出更合適的字詞，這些文字不會被儲存。系統啟用安全輸入時，AquaType 不會讀取，也不會儲存任何記錄。
- 若要清除個人記憶，請刪除「~/.aquatype」檔案夾中名稱以「personal_memory」開頭的檔案，然後登出並重新登入。

## 已知限制

- 僅支援標準注音鍵盤配置，並需搭配「ABC」或「美國」英文鍵盤配置使用。使用 Dvorak、法文等其他鍵盤配置時，注音的按鍵位置會不正確。
- 無法使用數字鍵選取候選。請使用方向鍵，或直接按一下候選。
- 輸入注音符號或特殊符號時，請使用「表情符號與符號」檢視器（按下 Fn-E 或地球鍵-E）。
- 輸入中無法將游標移到中間修改單一字詞。
- 每次輸入最多約 60 個字。達到上限後，新的注音按鍵不會被輸入，並會發出提示音。
- 句子越長，或按錯的鍵越多，候選中越可能沒有正確的結果。這時可以按 Tab 分段選字，或按 Delete 修改。
- 不支援「旁白」。

## 回報問題

請到 [Issues](../../issues) 回報問題，並附上 macOS 版本、Mac 機型，以及發生問題的 App。安全性問題請參閱 [SECURITY.md](SECURITY.md) 私下回報。

AquaType 是個人維護的專案，回覆可能需要幾天。如果想修改程式碼，請先開 Issue 討論。

## 授權與致謝

- AquaType 輸入法修改自[鼠鬚管（Squirrel）](https://github.com/rime/squirrel)，以 GNU 通用公共授權條款第 3 版（GPL-3.0）發布。
- AquaType 引擎與解碼器以 Apache 授權條款 2.0 版（Apache-2.0）發布。
- 模型以創用 CC 姓名標示-非商業性 4.0 國際授權條款（CC BY-NC 4.0）發布，僅限非商業用途。模型從零開始訓練，未使用任何預先訓練的模型；訓練資料請參閱 [MODEL.md](MODEL.md)。
- 字典資料來自多個開放來源，各依其授權發布。完整的授權條文與資料出處，請參閱 [NOTICE.txt](NOTICE.txt)。

AquaType 的開發使用了以下專案、資料與工具：

- [RIME](https://rime.im) 輸入法引擎與鼠鬚管，由佛振（Lotem）及貢獻者開發
- [小麥注音輸入法（McBopomofo）](https://github.com/openvanilla/McBopomofo)的注音資料
- 教育部《重編國語辭典修訂本》
- Apple 的 [MLX](https://github.com/ml-explore/mlx)
- [sc-dictionary](https://github.com/samejack/sc-dictionary)、[rime-terra-pinyin](https://github.com/rime/rime-terra-pinyin)、[pypinyin](https://github.com/mozillazg/python-pinyin) 等字詞資料
- [Claude Code](https://github.com/anthropics/claude-code)（Anthropic）與 [Codex](https://github.com/openai/codex)（OpenAI），協助撰寫程式碼與文件
