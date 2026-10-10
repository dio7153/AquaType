#!/usr/bin/env python3
"""Build the Rime dictionaries in packaging/payload/rime from openly licensed sources.

What it writes, and from what:

  aquatype_phrases       words from sc-dictionary (SJ Chou, CC BY 3.0 TW), Traditional Chinese only
  terra_pinyin           rime-terra-pinyin (LGPL-3.0) without Mainland and Japanese character forms, and with the
                         forms Taiwan does not use rewritten to Taiwan's (爲 as 為, 着 as 著, 裏 as 裡)
  aquatype_mcbopomofo    the McBopomofo table already in the payload, first tone written out, erhua as er5, without
                         Japanese forms
  aquatype_lexicon       the lexicon already in the payload: its words and weights, read again from the sources below
  aquatype_variants      other ways the words above are typed, at half their weight: a neutral tone with the
                         character's own tone (一個 as ㄧ ㄍㄜˋ), and 不 and 一 with their own tone where the word
                         writes tone sandhi (不要 as ㄅㄨˋ ㄧㄠˋ, 一定 as ㄧ ㄉㄧㄥˋ)
  aquatype.extended      the dictionary the schema converts with: imports the tables above and aquatype_boost

Readings come from, in order of preference:
  words       McBopomofo (MIT), the Ministry of Education's Revised Mandarin Chinese Dictionary (CC BY-ND 3.0 TW, via
              g0v/moedict-data, whose format conversion the Ministry allows), pypinyin's phrase table (MIT), Terra Pinyin
  characters  for a character McBopomofo's BPMFBase lists, its readings (and the Ministry's in syllables it lacks), the
              most frequent by Terra Pinyin first (a reading only Terra Pinyin has counts for the same syllable in
              Taiwan's tone); for others, Terra Pinyin by its frequencies, then pypinyin.  The Ministry's order of
              readings is not by use, so it never decides by itself
McBopomofo and the Ministry give Taiwan's readings; pypinyin and Terra Pinyin follow Mainland usage (tone sandhi written
in, more neutral tones, 期 as qi1), so they only fill in.

Weights: rows without one take librime's from Rime's word-frequency list (essay.txt), which counts words under OpenCC's
standard forms (爲 211329, 為 684).  A row in a form essay.txt counts under another spelling (為什麼, 隨著, terra's rows
rewritten to Taiwan's forms) is given the larger of the two counts as a weight of its own.

The first tone is always written out ("zhi1", not "zhi"): librime reads a syllable with no tone as one typed without a
tone key, so "zu3 zhi" is not found when ㄗㄨˇ ㄓ is typed with Space.  The fallback store strips that key again
(scripts/build_dict_compact_store.py), because the engine looks words up without it.

Inputs, as used for the files in the payload:
  McBopomofo          https://github.com/openvanilla/McBopomofo           f370963   Source/Data/BPMFBase.txt
  rime-terra-pinyin   https://github.com/rime/rime-terra-pinyin           723e51b
  sc-dictionary       https://github.com/samejack/sc-dictionary           e057977
  moedict-data        https://github.com/g0v/moedict-data                 dict-revised.json (.json.xz accepted)
  rime-essay          essay.txt as Squirrel ships it (third_party/squirrel/data/plum/essay.txt, the default)
  pypinyin 0.55.0, OpenCC (the Python package) for the Traditional-only, Japanese-form and Taiwan-form checks

  python3 packaging/build_rime_dicts.py --bpmfbase BPMFBase.txt --terra rime-terra-pinyin/ \\
      --sc-dictionary sc-dictionary/ --moedict dict-revised.json [--essay essay.txt] [--out packaging/payload/rime]

Running it again on its own output gives the same files.
"""
from __future__ import annotations

import argparse
import json
import lzma
import re
from collections import defaultdict
from pathlib import Path

import opencc
from pypinyin import Style, pinyin
from pypinyin.phrases_dict import phrases_dict

ROOT = Path(__file__).resolve().parents[1]
VERSION = "20261009"
SYLLABLE = re.compile(r"^[a-zv]+[1-5]$")
TONE_MARK = {"ˊ": "2", "ˇ": "3", "ˋ": "4", "˙": "5", "ˉ": "1"}
# The one lexicon entry whose readings are fixed by hand: both readings of 囉 are typed (a regression case).
FIXED_READINGS = {"是在哈囉": [("shi4", "zai4", "ha1", "luo2"), ("shi4", "zai4", "ha1", "luo5")]}
# 不 and 一 as words write them after tone sandhi, and the tone they are typed with as well.
SANDHI = {"不": ({"bu2"}, "bu4"), "一": ({"yi2", "yi4"}, "yi1")}


def syllable(p: str) -> str:
    """One syllable as the payload writes it: v for ü, er for erhua's r, the tone always a digit."""
    p = p.strip().lower().replace("ü", "v").replace("u:", "v")
    if re.fullmatch(r"r[1-5]?", p):
        p = "er" + p[1:]
    if p in ("er", "er1"):             # erhua without a tone, or with the first: ㄦ has no first tone
        return "er5"
    return p if p[-1:].isdigit() else p + "1"


def weight_of(value: str | None) -> float:
    if not value:
        return 1.0
    value = value.strip()
    try:
        return float(value[:-1]) / 100.0 if value.endswith("%") else float(value)
    except ValueError:
        return 1.0


def read_dict(path: Path):
    """(text, syllables, weight, has_weight) for each row of a Rime dictionary's body."""
    body = False
    for line in path.read_text(encoding="utf-8").splitlines():
        if line == "...":
            body = True
            continue
        if not body or not line or line.startswith("#"):
            continue
        f = line.split("\t")
        if len(f) >= 2 and f[1].strip():
            yield f[0], [syllable(s) for s in f[1].split()], weight_of(f[2] if len(f) > 2 else None), len(f) > 2


def header(name: str, comment: list[str], extra: list[str] = ()) -> list[str]:
    return ["# Rime dictionary", "# encoding: utf-8", *comment, "---", f"name: {name}", f'version: "{VERSION}"',
            "sort: by_weight", *extra, "..."]


def is_cjk(text: str) -> bool:
    return all(0x3400 <= ord(c) <= 0x9FFF or 0xF900 <= ord(c) <= 0xFAFF for c in text)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--bpmfbase", type=Path, required=True)
    ap.add_argument("--terra", type=Path, required=True)
    ap.add_argument("--sc-dictionary", type=Path, required=True)
    ap.add_argument("--moedict", type=Path, required=True)
    ap.add_argument("--essay", type=Path, default=ROOT / "third_party/squirrel/data/plum/essay.txt")
    ap.add_argument("--out", type=Path, default=ROOT / "packaging/payload/rime")
    a = ap.parse_args()
    out = a.out
    terra_path = a.terra / "terra_pinyin.dict.yaml"
    mcb = list(read_dict(out / "aquatype_mcbopomofo.dict.yaml"))
    lexicon = list(read_dict(out / "aquatype_lexicon.dict.yaml"))
    terra = list(read_dict(terra_path))
    essay: dict[str, float] = {}
    for line in a.essay.read_text(encoding="utf-8").splitlines():
        f = line.split("\t")
        if len(f) == 2:
            essay[f[0]] = float(f[1])

    # ---- character forms ------------------------------------------------------------------------------------------
    jp2t, s2t, t2s = opencc.OpenCC("jp2t"), opencc.OpenCC("s2twp"), opencc.OpenCC("tw2sp")
    t2tw, tw2t = opencc.OpenCC("t2tw"), opencc.OpenCC("tw2t")
    base = [l.split() for l in a.bpmfbase.read_text(encoding="utf-8").splitlines()]
    big5 = {f[0] for f in base if len(f) >= 5 and len(f[0]) == 1 and f[-1] == "big5"}
    model_chars = set(json.loads((ROOT / "decoder/spec/syllables_v1.json").read_text())["chars"])

    def japanese_form(text: str) -> bool:     # outside Big5 and changed by OpenCC's Japanese table: 亜, 観, 発
        return any(c not in big5 and jp2t.convert(c) != c for c in text)

    def traditional(text: str) -> bool:       # unchanged by Simplified-to-Taiwan, and back again
        return s2t.convert(text) == text and s2t.convert(t2s.convert(text)) == text

    # McBopomofo writes erhua as ㄦ with no tone mark, which writing the first tone out made er1; ㄦ has no first tone.
    def erhua(p: str) -> str:
        return "er5" if p == "er1" else p

    rows, seen = [], set()
    for t, p, w, has_w in mcb:
        p = [erhua(s) for s in p]
        if not japanese_form(t) and (t, tuple(p)) not in seen:      # 渋谷, 早稲田
            seen.add((t, tuple(p)))
            rows.append((t, p, w, has_w))
    mcb = rows

    # Forms Taiwan does not write (爲 着 裏 喫 羣 麪 牀 衆): the ones OpenCC's Taiwan table changes, unless McBopomofo's
    # words use a form at least a quarter as often as the one it would become (污 and 汙, 祕 and 秘 are both written).
    allowed = {c for c in model_chars | {f[0] for f in base if f and len(f[0]) == 1} if not japanese_form(c)}
    in_words = defaultdict(int)
    for t in {t for t, _, _, _ in mcb}:
        for c in set(t):
            in_words[c] += 1
    taiwan_char = {}
    for c in {c for t, _, _, _ in terra for c in t}:
        d = t2tw.convert(c)
        if d != c and len(d) == 1 and d in allowed and 4 * in_words[c] < in_words[d]:
            taiwan_char[c] = d

    def taiwan(text: str) -> str:
        return "".join(taiwan_char.get(c, c) for c in text)

    def essay_of(text: str) -> float:
        """librime's weight for a row without one, the word looked up under either spelling: essay.txt counts 為什麼 as
        爲什麼.  A character is looked up as written: 著 stands for 着 in some of its readings only."""
        w = essay.get(text, 0.0)
        return max(w, essay.get(tw2t.convert(text), 0.0)) if len(text) > 1 else w

    # Words only: a character rewritten keeps its own readings' frequencies, not lending them to the form it becomes
    # (蔘 shen1 is not 參's).
    terra = [(taiwan(t) if len(t) > 1 else t, p, w, has_w) for t, p, w, has_w in terra]

    # ---- readings -------------------------------------------------------------------------------------------------
    word_best: dict[str, tuple[list[str], tuple]] = {}
    char_best: dict[str, tuple[str, tuple]] = {}
    char_all: dict[str, set[str]] = defaultdict(set)

    def note_word(t, p, rank):
        if len(p) == len(t) and all(SYLLABLE.match(s) for s in p) and (t not in word_best or rank > word_best[t][1]):
            word_best[t] = (p, rank)

    def note_char(c, p, rank):
        if SYLLABLE.match(p):
            char_all[c].add(p)
            if c not in char_best or rank > char_best[c][1]:
                char_best[c] = (p, rank)

    for t, p, w, _ in mcb:
        if len(t) >= 2:
            note_word(t, p, (4, w))
    terra_frequency: dict[str, dict[str, float]] = defaultdict(dict)
    for t, p, w, _ in terra:
        if len(t) == 1:
            note_char(t, p[0], (3, w))
            terra_frequency[t][p[0]] = max(w, terra_frequency[t].get(p[0], 0.0))
        else:
            note_word(t, p, (1, w))
    base_readings: dict[str, list[str]] = defaultdict(list)
    for f in base:
        if len(f) >= 3 and len(f[0]) == 1:
            p = erhua(syllable(f[2]))
            note_char(f[0], p, (2, 0.0))
            if SYLLABLE.match(p) and p not in base_readings[f[0]]:
                base_readings[f[0]].append(p)
    for w in phrases_dict:
        if len(w) >= 2:
            note_word(w, [syllable(x[0]) for x in pinyin(w, style=Style.TONE3, neutral_tone_with_five=True)], (2, 0.0))
    # The Ministry's bopomofo becomes pinyin through the syllables the other sources already spell.
    pinyin_by_bopomofo: dict[str, str] = {}
    for c in list(char_all):
        for p in char_all[c]:
            pinyin_by_bopomofo.setdefault(_bopomofo_of(p), p)
    for p, _ in list(word_best.values()):
        for s in p:
            pinyin_by_bopomofo.setdefault(_bopomofo_of(s), s)
    moedict_chars: dict[str, list[str]] = defaultdict(list)
    moedict_bopomofo: dict[str, set[str]] = defaultdict(set)    # as the Ministry writes it, toneless: ㄌㄩㄢ
    moedict_raw = lzma.open(a.moedict).read() if a.moedict.suffix == ".xz" else a.moedict.read_bytes()
    for e in json.loads(moedict_raw):
        t = e.get("title", "")
        if not t or "{" in t or not is_cjk(t):
            continue
        for i, h in enumerate(e.get("heteronyms", [])):
            b = re.sub(r"（[^）]*）", "", h.get("bopomofo") or "").strip()
            if len(t) == 1 and b:
                moedict_bopomofo[t].add(_normal_bopomofo(b)[:-1])
            syl = [pinyin_by_bopomofo.get(_normal_bopomofo(x)) for x in re.split("[\\s\u3000]+", b) if x]
            if not b or len(syl) != len(t) or None in syl:
                continue
            if len(t) == 1:
                note_char(t, syl[0], (0, -i))
                moedict_chars[t].append(syl[0])
            else:
                note_word(t, syl, (3, -i))
    # A character McBopomofo lists is read only as McBopomofo reads it, or as the Ministry does in a syllable McBopomofo
    # does not have (the Ministry's other tones are often a second reading, 期 qi1): Terra Pinyin alone reads 期 qi1,
    # 微 wei1 and 息 xi1, as the Mainland does.  Among them, the one Terra Pinyin finds most frequent, a reading of its
    # that is not among them counting for the one of the same syllable in another tone (識 shi2 for shi4); in a tie
    # Terra Pinyin's first reading, then one of McBopomofo's that Terra Pinyin has as it is (埔 pu3, not bu3 for its
    # bu4), then McBopomofo's order.  Where the Ministry does not
    # have the character and Terra Pinyin and pypinyin agree on a syllable McBopomofo does not give at all (唑:
    # McBopomofo ㄕˋ, both of them zuo4), McBopomofo is not taken for the character's reading.
    for c, readings in base_readings.items():
        moe = list(dict.fromkeys(moedict_chars.get(c, ())))
        first = syllable(pinyin(c, style=Style.TONE3, neutral_tone_with_five=True)[0][0])
        if not moedict_bopomofo[c] and any(first[:-1] == q[:-1] for q in terra_frequency[c]) \
                and not any(p[:-1] == q[:-1] for p in readings for q in [first, *terra_frequency[c]]):
            continue
        own = len(readings)                     # McBopomofo's own; the Ministry's follow
        readings = readings + [p for p in moe if all(p[:-1] != q[:-1] for q in readings)]
        frequency = {p: terra_frequency[c].get(p, -1.0) for p in readings}
        for r, w in terra_frequency[c].items():
            for p in readings:
                if r not in frequency and p[:-1] == r[:-1]:
                    frequency[p] = max(frequency[p], w)
        best = max(readings, key=lambda p: (frequency[p], p == char_best[c][0],
                                            readings.index(p) < own and p in terra_frequency[c], -readings.index(p)))
        char_best[c] = (best, (5, 0.0))

    def taiwan_word(word: str) -> list[str]:
        """A known word's reading.  One that only pypinyin or Terra Pinyin gives can read a character as the Mainland
        does (冰川期 qi1): such a syllable, when neither McBopomofo nor the Ministry reads the character so, becomes the
        same syllable in a tone they give (的士 di2), else the character's own reading.  Neutral tones and 不 and 一
        (tone sandhi) are left as they are."""
        p, rank = word_best[word]
        if rank[0] >= 3:
            return p
        out_syl = []
        for c, syl in zip(word, p):
            allowed = list(dict.fromkeys([*base_readings.get(c, ()), *moedict_chars.get(c, ())]))
            if allowed and syl not in allowed and not syl.endswith("5") and c not in SANDHI:
                same = sorted((q for q in allowed if q[:-1] == syl[:-1]), key=lambda q: q != char_best.get(c, ("",))[0])
                if same:
                    syl = same[0]
                elif char_best.get(c, ("",))[0] in allowed:
                    syl = char_best[c][0]
            out_syl.append(syl)
        return out_syl

    def read(text: str) -> list[str]:
        """The reading of a word: the longest known words from the left, else each character's own reading."""
        if text in word_best:
            return taiwan_word(text)
        out_syl, pos = [], 0
        while pos < len(text):
            for size in range(min(6, len(text) - pos), 1, -1):
                if text[pos:pos + size] in word_best:
                    out_syl += taiwan_word(text[pos:pos + size])
                    pos += size
                    break
            else:
                c = text[pos]
                if c not in char_best:
                    for p in pinyin(c, style=Style.TONE3, heteronym=True, neutral_tone_with_five=True)[0]:
                        note_char(c, syllable(p), (1, 0.0))
                if c not in char_best:
                    return []
                out_syl.append(char_best[c][0])
                pos += 1
        return out_syl

    written: dict[str, list[tuple[str, list[str], float, bool]]] = {}

    def write(name: str, lines: list[str], rows: list[tuple[str, list[str], float, bool]]) -> None:
        for t, p, w, has_w in rows:
            lines.append(f"{t}\t{' '.join(p)}" + (f"\t{int(w) if w.is_integer() else w!r}" if has_w else ""))
        (out / f"{name}.dict.yaml").write_text("\n".join(lines) + "\n", encoding="utf-8")
        written[name] = rows

    # ---- aquatype_mcbopomofo: the same rows, first tone written out --------------------------------------------------
    write("aquatype_mcbopomofo", header("aquatype_mcbopomofo", [
        "# Generated by AquaType from McBopomofo's data (https://github.com/openvanilla/McBopomofo), MIT License."]), mcb)

    # ---- aquatype_lexicon: same words and weights, readings from the sources above -----------------------------------
    weights: dict[str, float] = {}
    for t, _, w, _ in lexicon:
        weights[t] = max(weights.get(t, 0.0), w)
    rows = []
    for t in sorted(weights, key=lambda t: (-weights[t], len(t), t)):
        if japanese_form(t):
            continue
        readings = [tuple(r) for r in FIXED_READINGS.get(t, [])] or ([tuple(read(t))] if read(t) else [])
        rows += [(t, list(r), weights[t], True) for r in readings]
    write("aquatype_lexicon", header("aquatype_lexicon", [
        "# Generated by AquaType from the coverage word inventory, whose sources NOTICE.txt lists - CC BY-SA ones among",
        "# them - and distributed under CC BY-SA 3.0."]),
          rows)

    # ---- aquatype_phrases: sc-dictionary's Traditional words -------------------------------------------------------
    # A word essay.txt counts under its other spelling (為什麼 as 爲什麼) carries that count as its weight.
    rows, seen_phrases = [], set()
    for t in (a.sc_dictionary / "main.txt").read_text(encoding="utf-8").splitlines():
        t = t.strip()
        if t not in seen_phrases and 2 <= len(t) <= 12 and is_cjk(t) and not japanese_form(t) and traditional(t) \
                and (r := read(t)):
            seen_phrases.add(t)
            rows.append((t, r, essay_of(t), essay_of(t) > essay.get(t, 0.0)))
    write("aquatype_phrases", header("aquatype_phrases", [
        "# Words from sc-dictionary (https://github.com/samejack/sc-dictionary) by SJ Chou, CC BY 3.0 TW: its",
        "# Traditional Chinese words, read with McBopomofo, the Ministry of Education's dictionary, pypinyin and",
        "# rime-terra-pinyin.  The weights some rows carry are the counts of Rime's essay.txt (rime-essay, LGPL-3.0)."]), rows)

    # ---- terra_pinyin: Rime's table without Mainland and Japanese forms, in Taiwan's forms -----------------------------
    # A row rewritten to Taiwan's form (爲 wei4 as 為 wei4) takes librime's weight for the row it was, looked up under
    # either form, written out, since essay.txt has it under the old one; where the Taiwan form already has a row with
    # the same reading, the two become one with the higher weight (着 zhe5 85% and 著 zhe5 0% become 著 zhe5 80336).
    # So does a word essay.txt counts under its other spelling.  A character takes the count of the form it was only:
    # essay.txt's 參 is mostly can1, and 蔘 shen1 is not.
    def terra_weight(t: str, tw: str, w: str) -> float:
        count = essay_of(t) if len(t) == 1 else max(essay_of(t), essay_of(tw))
        return count * float(w[:-1]) / 100 if w.endswith("%") else float(w) if w else count

    terra_lines = terra_path.read_text(encoding="utf-8").splitlines(keepends=True)
    terra_rows: list[tuple[int, str, str, str]] = []      # (line, Taiwan form, code, weight) for each row kept
    weighted: dict[tuple[str, str], float] = {}
    body = False
    for i, line in enumerate(terra_lines):
        if body and "\t" in line and not line.startswith("#"):
            f = line.rstrip("\n").split("\t")
            if not all(c in allowed for c in f[0]):
                continue
            tw, code, w = taiwan(f[0]), f[1], f[2] if len(f) > 2 else ""
            terra_rows.append((i, tw, code, w))
            if tw != f[0] or (len(tw) > 1 and not (w and not w.endswith("%")) and essay_of(tw) > essay.get(tw, 0.0)):
                weighted[(tw, code)] = 0.0
        body = body or line.startswith("...")
    for i, tw, code, w in terra_rows:
        if (tw, code) in weighted:
            weighted[(tw, code)] = max(weighted[(tw, code)], terra_weight(terra_lines[i].split("\t")[0], tw, w))
    replaced: dict[int, str | None] = {}
    terra_written, done = [], set()
    for i, tw, code, w in terra_rows:
        if (tw, code) in weighted:
            replaced[i] = None if (tw, code) in done else f"{tw}\t{code}\t{round(weighted[(tw, code)])}\n"
            if (tw, code) not in done:
                terra_written.append((tw, [syllable(s) for s in code.split()], weighted[(tw, code)], True))
            done.add((tw, code))
        else:
            terra_written.append((tw, [syllable(s) for s in code.split()], terra_weight(tw, tw, w), True))
    kept, body, rows_at = [], False, {i for i, _, _, _ in terra_rows}
    for i, line in enumerate(terra_lines):
        if body and "\t" in line and not line.startswith("#") and i not in rows_at:
            continue
        body = body or line.startswith("...")
        if replaced.get(i, line) is not None:
            kept.append(replaced.get(i, line))
    (out / "terra_pinyin.dict.yaml").write_text(
        "".join(kept).replace("# Rime dictionary\n", "# Rime dictionary\n"
                              "# rime-terra-pinyin (https://github.com/rime/rime-terra-pinyin), LGPL-3.0 (its readings from\n"
                              "# CC-CEDICT, CC BY-SA 3.0, as its own header below says), modified by\n"
                              f"# AquaType on {VERSION[:4]}-{VERSION[4:6]}-{VERSION[6:]}: without the characters that are "
                              "neither in AquaType's model nor in\n# McBopomofo, and without Japanese forms; forms Taiwan "
                              "does not use rewritten to Taiwan's (爲 as 為,\n# 着 as 著), with weights of their own from "
                              "Rime's essay.txt.\n", 1),
        encoding="utf-8")
    written["terra_pinyin"] = terra_written

    # ---- aquatype_variants: other ways the words are typed (一個 as ㄧ ㄍㄜˋ, 不要 as ㄅㄨˋ ㄧㄠˋ) --------------------
    def own_tone(c: str, neutral: str) -> str | None:
        """The same syllable with a tone of its own (ge5 -> ge4), not another reading of the character: 的 is de5 or
        di2/di4, and di2 is a different syllable, not 的 typed with a tone."""
        body = neutral[:-1]
        for p in sorted(char_all.get(c, ()), key=lambda p: (p != char_best.get(c, ("",))[0], p)):
            if p[:-1] == body and not p.endswith("5"):
                return p
        return None

    def with_own_tone(t: str, p: tuple) -> tuple:
        return tuple(own_tone(c, s) if s.endswith("5") else s for c, s in zip(t, p))

    def without_sandhi(t: str, p: tuple) -> tuple:
        return tuple(SANDHI[c][1] if c in SANDHI and s in SANDHI[c][0] else s for c, s in zip(t, p))

    sources = ("aquatype_mcbopomofo", "aquatype_lexicon", "aquatype_phrases", "terra_pinyin")
    # Not terra_pinyin's rows: one of those takes only its essay.txt weight, often far below half the word's here.
    have = {(t, tuple(p)) for name in sources[:3] for t, p, _, _ in written[name]}
    rows, seen = [], set()
    for name in sources:
        for t, p, w, has_w in written[name]:
            if len(t) < 2:
                continue
            p = tuple(p)
            # The neutral tone from AquaType's own tables only: Terra Pinyin writes it where Taiwan does not.
            toned = with_own_tone(t, p) if name != "terra_pinyin" and any(s.endswith("5") for s in p) else None
            for q in (toned, without_sandhi(t, p), without_sandhi(t, toned) if toned and None not in toned else None):
                if q is None or None in q or q == p or (t, q) in have or (t, q) in seen:
                    continue
                seen.add((t, q))
                # Below the word it comes from: an alternative way to type it, never a reason to prefer it over another
                # word.  Written out, since a row without a weight would take the word's whole weight from essay.txt.
                rows.append((t, list(q), (w if has_w else essay_of(t)) / 2, True))
    write("aquatype_variants", header("aquatype_variants", [
        "# Other ways the words of AquaType's tables are typed, at half the word's weight: a neutral tone with the",
        "# character's own tone (一個 as ㄧ ㄍㄜˋ as often as ㄧ ㄍㄜ˙), and 不 and 一 with their own tone where the word",
        "# writes tone sandhi (不要 as ㄅㄨˋ ㄧㄠˋ, 一定 as ㄧ ㄉㄧㄥˋ).  Made from aquatype_mcbopomofo, aquatype_lexicon,",
        "# aquatype_phrases and terra_pinyin, and distributed under CC BY-SA 3.0 as the lexicon is; the rows made from",
        "# terra_pinyin's words are also under its LGPL-3.0."]), rows)

    # ---- aquatype.extended: what the schema converts with ----------------------------------------------------------
    (out / "aquatype.extended.dict.yaml").write_text("\n".join(header("aquatype.extended", [
        "# The dictionary AquaType's Rime schema converts with: AquaType's own tables and the ones it imports."], [
        "# Entries without a weight of their own take it from Rime's word-frequency list (essay.txt, shipped in",
        "# SharedSupport).  Without it they all weigh 0, and a rare character with any weight at all goes first.",
        "# Entries in a form essay.txt counts under another spelling (為什麼 as 爲什麼) carry their weight.",
        "vocabulary: essay",
        "import_tables:",
        *(f"  - {n}" for n in ("aquatype_lexicon", "aquatype_boost", "aquatype_mcbopomofo", "terra_pinyin",
                               "aquatype_phrases", "aquatype_variants"))])) + "\n", encoding="utf-8")
    print(json.dumps({n: len(r) for n, r in written.items()}))


_KEYS = dict(zip("bpmfdtnlgkhjqxZCSrzcsiuvaoeEAIOUMNKGR", "ㄅㄆㄇㄈㄉㄊㄋㄌㄍㄎㄏㄐㄑㄒㄓㄔㄕㄖㄗㄘㄙㄧㄨㄩㄚㄛㄜㄝㄞㄟㄠㄡㄢㄣㄤㄥㄦ"))


def _bopomofo_of(p: str) -> str:
    """A pinyin syllable as bopomofo with its tone digit: zhi1 -> ㄓ1."""
    body, tone = p[:-1], p[-1]
    for pattern, replacement in ((r"iu", "iou"), (r"ui", "uei"), (r"ong", "ung"), (r"yi?", "i"), (r"wu?", "u"),
                                 (r"iu", "v"), (r"([jqx])u", r"\1v"), (r"([iuv])n", r"\1en"), (r"zhi?", "Z"),
                                 (r"chi?", "C"), (r"shi?", "S"), (r"([zcsr])i", r"\1"), (r"ai", "A"), (r"ei", "I"),
                                 (r"ao", "O"), (r"ou", "U"), (r"ang", "K"), (r"eng", "G"), (r"an", "M"), (r"en", "N"),
                                 (r"er", "R"), (r"eh", "E"), (r"([iv])e", r"\1E")):
        body = re.sub(pattern, replacement, body)
    return "".join(_KEYS.get(ch, "?") for ch in body) + tone


def _normal_bopomofo(s: str) -> str:
    """The Ministry's bopomofo for one syllable (tone mark before or after, none for the first tone) as ㄓ1."""
    tone = "1"
    for mark, digit in TONE_MARK.items():
        if mark in s:
            tone, s = digit, s.replace(mark, "")
    return s + tone


if __name__ == "__main__":
    main()
