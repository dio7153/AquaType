# 解碼器原生層（decoder/native）

App 的引擎（`Sources/AquaTypeEngine`）透過 C API 呼叫這一層：把注音按鍵串解成候選句子。模型在 MLX 上執行，不經過 Python。

## 檔案

| 檔案 | 內容 |
| --- | --- |
| `aquadecoder.h` / `aquadecoder.cpp` | C API（`Sources/AquaTypeDecoderBridge/include/aquadecoder.h` 是同一份的副本，兩者必須完全相同） |
| `v2_session.h` / `v2_session.cpp` | 解碼流程：列候選 → 音節模型打分 → 與字模型聯合的 beam search → 候選 |
| `v2_model.h` / `v2_model.cpp` | 兩個模型的 MLX 實作，以及自製的 Metal kernel |
| `enumerate.*` / `edit.*` | 前端：按鍵分段、容錯的編輯距離與通道成本（與 `decoder/core/ref_edit.py` 逐位元一致） |
| `v2_main.cpp` | `aq_v2`：延遲與記憶體量測工具 |
| `bindings.cpp` / `__init__.py` | Python 模組 `decoder_native`（訓練與研究用） |
| 其餘 `.cpp` / `.h` | v1 研究核心（`decoder/core/types.py` 的物件），不進 app |

## C API

所有回傳的字串都由 `malloc` 配置，用 `aq_free` 釋放。失敗時回傳 `NULL`，原因可用 `aq_last_error()` 取得。

| 函式 | 用途 |
| --- | --- |
| `aq_open(model_dir, metallib_path)` | 開啟一個 v2 匯出目錄（`decoder/v2/export_v2.py` 產生，model.json 內含 `"format": "aquatype-v2/1"`）；其他格式一律拒絕 |
| `aq_close(d)` | 關閉 |
| `aq_decode(d, keys, ctx_json)` | 解碼，回傳 `{"candidates":[{text, score, fit_total, char_ends, reading, prefix}], "timing_ms":{...}, "counts":{...}, "prof":{...}}` |
| `aq_reset(d)` | 一句結束（commit 或換了上下文）：丟掉逐鍵之間保留的狀態 |
| `aq_maintain(d)` | 兩鍵之間的整理（壓縮 K/V 表、把空閒記憶體還給系統）；在送出候選**之後**呼叫，才不會算進使用者等待的時間 |
| `aq_readings(d, keys)` | 乾淨按鍵串對應的音節 id（JSON 陣列）；無法精確分段時回傳 `[]` |
| `aq_char_id(d, utf8_char)` | 字的 id；字表外回傳 −1 |
| `aq_free`、`aq_last_error`、`aq_version` | 釋放字串、最近的錯誤、版本字串 |

`ctx_json` 是上下文字元：`[{"kind":0,"chars":[id],"weights":[1.0]}, ...]`，最多取最後 64 個。

## 每一鍵的流程

1. **列候選**：每個結尾鍵往前試 1 到 6 鍵的每一段，找出編輯數在上限內的音節，並附上通道成本。結果依按鍵片段快取。
2. **音節模型**：一次算出整串按鍵的隱藏狀態，用 span head 替每一段打分；溫度依「有幾成起點有零成本的弧」調整。
3. **剪枝**：每個起點保留分數最高的弧，低於下限的丟掉；字頭在同音字群組內替每個字打分。
4. **Beam search**：逐位置用字模型接下一個字，透過樹狀 K/V 表讓共同前綴只算一次。距離結尾 12 鍵以前的位置沿用上一鍵的 beam。
5. **結尾**：加上句尾機率，取前 256 個不重複的句子。

正式版的參數集中在 `v2_session.h`，每一個都附有理由。

## 編譯

```sh
bash decoder/native/build.sh [--mlx] [--dylib] [--v2cli] [--cli] [--check]
```

- 不加參數：只編 Python 模組 `decoder_native`（不含 MLX）。
- `--mlx`：模組連同 MLX 解碼器一起編。
- `--dylib`：另外編 `libaquadecoder.dylib`（只用 v2 的原始碼），`packaging/build_app.sh` 呼叫的就是這個。
- `--v2cli`：另外編 `aq_v2`。
- `--cli`：另外編 v1 的 `aq_latency`。

## 診斷

- `AQ_V2_TIME=1`：每一鍵在 stderr 印一行各階段耗時。
- `AQ_LOG_MS=1`：每次 `aq_decode` 在 stderr 印出解碼器自己算的總時間與各階段時間，用來把引擎量到的來回時間拆成「解碼器」與「其他」。

## 延遲量測

```sh
aq_v2 <model_dir> <windows.jsonl> [--n N] [--metallib path] [--cold-only 1] [--typed-only 1]
```

`windows.jsonl` 每行一個 `{"keys":"...","cell":20}`。每個視窗逐鍵輸入並在每鍵之後解碼（和產品打字時相同），記錄最後一鍵的延遲與整串冷解碼的延遲，依 cell 輸出百分位數，以及程序的實體記憶體用量（phys_footprint）。
