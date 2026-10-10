# AquaType 模型說明

**中文** | [English](MODEL.en.md)

本文件說明 AquaType 0.9.0 內建的兩個模型：注音模型與語言模型。

## 模型資訊

- **開發者：** Dio Hsieh
- **版本：** AquaType 0.9.0
- **用途：** 將注音按鍵轉換成繁體中文字詞，並在按鍵有誤時列出正確的字詞
- **語言：** 繁體中文（臺灣）
- **授權：** [CC BY-NC 4.0](https://creativecommons.org/licenses/by-nc/4.0/)，僅限非商業用途
- **基礎模型：** 無。兩個模型皆從零開始訓練，未使用任何預先訓練的模型或外部權重
- **原始碼：** 本 repository

## 模型架構

AquaType 使用兩個模型：

- **注音模型：** 讀取整串按鍵，評估每種注音切分的可能性，並估計每個位置可能的字。
- **語言模型：** 逐字評估候選字詞是否通順。

| | 注音模型 | 語言模型 |
|---|---|---|
| 架構 | 雙向 Transformer 編碼器 | 自迴歸 Transformer（僅解碼器） |
| 參數量 | 約 1.76 億 | 約 1,630 萬 |
| 層數／寬度／注意力頭 | 12 層／1024／16 | 6 層／384／6 |
| 輸入 | 注音按鍵（44 種） | 繁體中文字 |
| 輸出 | 1,784 個注音音節與 14,513 個字的機率 | 下一個字的機率（14,517 個詞彙） |
| 數值格式 | 4 位元量化（線性層） | 16 位元浮點數 |

兩個模型合計約 136 MB，以 [MLX](https://github.com/ml-explore/mlx) 在 Apple 晶片上執行，不需要網路。

## 用途

**預期用途**

- 在 AquaType 中，以標準注音鍵盤輸入繁體中文。
- 在按鍵有誤時，列出使用者原本想輸入的字詞。

**不適用的用途**

- 生成文字或對話。模型只在使用者輸入的按鍵範圍內評估候選，不會自行產生內容。
- 簡體中文、漢語拼音，或標準注音以外的鍵盤配置。
- 商業用途。授權僅允許非商業用途。

## 訓練

**訓練資料**

兩個模型以相同的 16 個公開資料來源訓練，皆為繁體中文文字：

- 注音模型：約 4,281 萬句、4.7 億字。
- 語言模型：約 1,845 萬句、1.9 億字。

| 資料來源 | 內容 | 授權 |
|---|---|---|
| [HPLT/HPLT2.0_cleaned](https://huggingface.co/datasets/HPLT/HPLT2.0_cleaned)（繁體中文） | 網頁文字 | CC0（資料集）；原始網頁各依其權利 |
| [botp/yentinglin-zh_TW_c4](https://huggingface.co/datasets/botp/yentinglin-zh_TW_c4) | 網頁文字（mC4 臺灣子集） | ODC-By |
| [jed351/finepdfs-traditional-chinese](https://huggingface.co/datasets/jed351/finepdfs-traditional-chinese) | PDF 文件文字（FinePDFs 繁體中文篩選） | ODC-By |
| [liswei/news-collection-zhtw](https://huggingface.co/datasets/liswei/news-collection-zhtw) | 新聞 | 未標示授權 |
| [Blaze7451/Wiki-zhtw-20250601](https://huggingface.co/datasets/Blaze7451/Wiki-zhtw-20250601) | 維基百科（臺灣正體） | CC BY-SA 4.0 |
| [erhwenkuo/zhwikisource-zhtw](https://huggingface.co/datasets/erhwenkuo/zhwikisource-zhtw) | 維基文庫 | CC BY-SA 3.0 |
| [erhwenkuo/wikinews-zhtw](https://huggingface.co/datasets/erhwenkuo/wikinews-zhtw) | 維基新聞 | CC BY-SA 3.0（資料集標示） |
| [zake7749/Gossiping-Chinese-Corpus](https://github.com/zake7749/Gossiping-Chinese-Corpus) | PTT 推文 | Apache-2.0（資料集）；內容著作權屬原作者 |
| [yuhuanstudio/PTT-pretrain-zhtw](https://huggingface.co/datasets/yuhuanstudio/PTT-pretrain-zhtw) | PTT 文章 | Apache-2.0（資料集）；內容著作權屬原作者 |
| [Riyuechang/PTT-Corpus-100K_Gossiping-1400-39400_v2](https://huggingface.co/datasets/Riyuechang/PTT-Corpus-100K_Gossiping-1400-39400_v2) | PTT 問答 | Apache-2.0（資料集）；內容著作權屬原作者 |
| [liao961120/dcard-corpus](https://github.com/liao961120/dcard-corpus) | Dcard 文章 | 未標示授權；內容著作權屬原作者 |
| [iddle/dcard_conversations](https://huggingface.co/datasets/iddle/dcard_conversations) | Dcard 對話 | MIT（資料集）；內容著作權屬原作者 |
| [erhwenkuo/moss-003-sft-chinese-zhtw](https://huggingface.co/datasets/erhwenkuo/moss-003-sft-chinese-zhtw) | 對話（由語言模型生成） | CC BY-NC 4.0 |
| [yentinglin/twllm-data](https://huggingface.co/datasets/yentinglin/twllm-data) | 對話（部分由語言模型生成） | Apache-2.0 |
| [OPUS OpenSubtitles v2018](https://opus.nlpl.eu/OpenSubtitles/corpus/version/OpenSubtitles)（zh_TW） | 影視字幕 | 僅限研究與非營利用途 |
| [adi-gov-tw/Taiwan-Tongues-ASR-CE-dataset-zhtw](https://huggingface.co/datasets/adi-gov-tw/Taiwan-Tongues-ASR-CE-dataset-zhtw) | 朗讀句子 | TRAIL-D（臺灣負責任 AI 授權 v0.1） |

各資料來源的授權，為其資料集頁面所標示的授權。網頁類資料（HPLT、mC4、FinePDFs）的原始網頁，各依其原始權利。

**訓練過程**

- 訓練用的注音按鍵，由文字依讀音轉換而成，不使用任何真實使用者的輸入。
- 注音模型訓練時，約四分之三的句子會以規則模擬按錯、漏按、多按與相鄰兩鍵按反，每個按鍵出錯的機率介於 0% 到 60%；語言模型只以文字訓練。
- 注音模型訓練 380,000 步，語言模型訓練 200,000 步。
- 訓練前已移除與測試集相同，或有連續 8 個字相同的句子。

## 評估

**測試資料：** 測試集共 4,049 句，在訓練前從相同的資料來源中，依句子長度分層抽出並保留。訓練資料已移除與測試集相同、或有連續 8 個字相同的句子；訓練完成後再次檢查，沒有重複。

測試集不公開：部分句子的來源不允許再次散布；保留一份未公開的測試集，也才能公平比較之後的版本。

**測試方式：** 每個按鍵各有一定機率被模擬為按錯、漏按、多按或與相鄰鍵按反。未使用個人記憶與前文。

**指標：** 正確的完整句子排在第一個、前 9 個（第一頁），以及前 64 個候選中的比例。

**結果**

以 AquaType 0.9.0 實際輸入的設定，測試從測試集隨機抽出的 600 句：

| 按錯的按鍵比例 | 正確結果排第一 | 正確結果在前 9 個 | 正確結果在前 64 個 |
|---|---|---|---|
| 0% | 78.7% | 94.3% | 97.3% |
| 20% | 52.8% | 75.8% | 84.2% |
| 40% | 33.5% | 51.7% | 63.7% |

**摘要：** 沒有按錯鍵時，正確句子約有 94% 出現在第一頁。按錯的鍵越多、句子越長，正確結果越可能不在候選中。

## 偏差、風險與限制

- 訓練資料包含大量論壇文字（PTT、Dcard）與部分由語言模型生成的對話，模型的用字與用詞會偏向這些來源。
- 句子越長，或按錯的鍵越多，候選中越可能沒有正確的結果。
- 部分口語用字與較少見的字詞，模型較少預測，可能排在較後面的候選中。
- 訓練資料中少見的領域、專有名詞或新詞，模型可能無法正確轉換。
- 評估中的按錯鍵都是以規則模擬，尚未以真實使用者的打字資料評估；測試句與訓練資料來自相同的來源，實際使用時的表現可能不同。

**建議：** 候選中沒有正確的結果時，請按 Tab 分段選字。AquaType 的個人記憶會記住你選過的字詞，下次輸入時優先列出。

## 授權

模型權重以創用 CC 姓名標示-非商業性 4.0 國際授權條款（CC BY-NC 4.0）發布，僅限非商業用途。部分訓練資料僅授權非商業使用，因此模型也僅供非商業用途。完整的授權與出處，請參閱 [NOTICE.txt](NOTICE.txt)。

## 聯絡

如果對模型或本文件有任何問題，請到 [Issues](../../issues) 回報。

如果你是訓練資料的權利人，希望你的內容不再用於訓練，請透過 [Issues](../../issues) 聯絡，會盡快回覆並處理。
