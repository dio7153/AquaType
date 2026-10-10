"""P3.2 (revised 2026-09-06): per-key typing simulator (spec window.*, window.frozen_set, eval.long_sentence_protocol).

Keys arrive one at a time; the window is the unfrozen tail (<= 36 keys).  When a new key would not fit, the leading
character of the CURRENT first-ranked candidate leaves the window and is frozen as a homophone SET: the characters
found at that position across the top-N candidates of the last decode (N = set_source: 64 or 256), weighted by
softmax(score), at most max_chars.  The set is never revised.  The model input for later windows carries one
soft-frozen context token per frozen position (chars + weights).  The last decode writes complete sentences: each
Candidate carries `prefix` (one char chosen from each frozen set) and `text` (the window's chars); the sentence is
prefix + text.  A sentence is a hit at rank r iff prefix + text equals the gold at rank r.  If a gold char is absent
from its frozen set the sentence cannot be recovered: `set_miss` reports that separately (the compounding cost).

Decoder interface (pure): decode(Window) -> list[Candidate] ranked, possibly longer than 64 (the extra are used only
to build sets); Candidate.char_ends are the key indices (within the window) each window char ends on.
"""
from __future__ import annotations
import collections, math, time
from dataclasses import dataclass, field
from typing import Callable, Dict, List, Optional, Sequence
import numpy as np
from decoder.core.types import Assembly, CtxToken, Pruned, Ranked, Window

@dataclass
class Candidate:
    text: str                                # window chars
    score: float
    fit_total: int
    char_ends: List[int]                     # per window char, 0-based key index it ends on, strictly increasing, last == n_keys - 1
    prefix: str = ""                         # one char per frozen position (len == number of frozen ctx tokens), chosen by the decoder
    reading: Optional[tuple] = None          # syllable ids per window char (reading-diversity stats)
    @property
    def full(self) -> str: return self.prefix + self.text
    def check(self, n_keys: int, n_frozen: int = 0) -> None:
        assert len(self.char_ends) == len(self.text) and len(self.text) >= 1
        assert all(b > a for a, b in zip(self.char_ends, self.char_ends[1:])) and self.char_ends[-1] == n_keys - 1 and self.char_ends[0] >= 0
        assert len(self.prefix) == n_frozen, f"prefix length {len(self.prefix)} != frozen positions {n_frozen}"

DecodeFn = Callable[[Window], List[Candidate]]

def candidates_from_assembly(asm: Assembly, pr: Pruned, ranked: Ranked, prefix_by_text: Optional[Dict[str, str]] = None) -> List[Candidate]:
    by_text: Dict[str, tuple] = {}
    for nodes, chars in zip(asm.sent_nodes, asm.sent_chars): by_text.setdefault(chars, ([int(pr.node_end[i]) for i in nodes], tuple(int(pr.node_syl[i]) for i in nodes)))
    return [Candidate(text=t, score=float(s), fit_total=int(f), char_ends=by_text[t][0], reading=by_text[t][1], prefix=(prefix_by_text or {}).get(t, "")) for t, s, f in ranked]

def frozen_set(cands: Sequence[Candidate], char_index: int, max_chars: int) -> CtxToken:
    """chars at char_index across the candidates, weighted by softmax(score); top max_chars"""
    m = max(c.score for c in cands); w: Dict[str, float] = collections.defaultdict(float)
    for c in cands:
        if char_index < len(c.text): w[c.text[char_index]] += math.exp(c.score - m)
    chars = sorted(w, key=lambda ch: -w[ch])[:max_chars]; z = sum(w[ch] for ch in chars)
    return CtxToken(kind="frozen", chars=chars, weights=[w[ch] / z for ch in chars])

@dataclass
class Snapshot:
    frozen_keys: int
    window_keys: str
    frozen_ctx: List[CtxToken]
    cands: List[Candidate]
    latency_ms: float

@dataclass
class Result:
    gold: str
    keys: str
    candidates: List[str]                    # final complete sentences (prefix + window text), ranked
    frozen_sets: List[CtxToken]
    frozen_positions: List[int]              # 1-based index of the key that pushed each char out
    latencies_ms: List[float]
    n_backspaces: int = 0
    last_distinct_readings: Optional[int] = None
    last_max_reading_share: Optional[float] = None
    @property
    def n_frozen(self) -> int: return len(self.frozen_sets)
    @property
    def set_miss(self) -> bool:
        """a gold char is absent from its frozen set: unrecoverable by construction"""
        return any(i < len(self.gold) and self.gold[i] not in t.chars for i, t in enumerate(self.frozen_sets)) or len(self.frozen_sets) > len(self.gold)
    @property
    def rank(self) -> Optional[int]: return self.candidates.index(self.gold) + 1 if self.gold in self.candidates else None

class Simulator:
    def __init__(self, decode: DecodeFn, max_keys: int = 36, context_max_tokens: int = 64, prev_sentence: Optional[str] = None, top: int = 64, decode_from_key: int = 1, set_source: int = 256, set_max_chars: int = 8):
        self.decode = decode; self.max_keys = max_keys; self.ctx_max = context_max_tokens; self.top = top; self.decode_from_key = decode_from_key; self.set_source = set_source; self.set_max_chars = set_max_chars
        self.prev_ctx = [CtxToken(kind="prev_sentence", chars=[ch], weights=[1.0]) for ch in (prev_sentence or "")]
        self.frozen_keys = 0; self.window_keys = ""; self.frozen_ctx: List[CtxToken] = []; self.cands: List[Candidate] = []
        self.snapshots: List[Snapshot] = []; self.frozen_positions: List[int] = []; self.latencies: List[float] = []; self.all_keys = ""; self.n_backspaces = 0
    def _ctx(self) -> List[CtxToken]: return (self.prev_ctx + self.frozen_ctx)[-self.ctx_max:]
    def _freeze_until_fits(self, incoming: str) -> None:
        while len(self.window_keys) + len(incoming) > self.max_keys:
            if not self.cands: raise RuntimeError("cannot freeze: no candidates for the current window")
            best = self.cands[0]; n_consumed = best.char_ends[0] + 1
            self.frozen_ctx.append(frozen_set(self.cands[:self.set_source], 0, self.set_max_chars)); self.frozen_keys += n_consumed; self.frozen_positions.append(len(self.all_keys))
            self.window_keys = self.window_keys[n_consumed:]
            shifted = [Candidate(c.text[1:], c.score, c.fit_total, [e - n_consumed for e in c.char_ends[1:]], c.prefix + c.text[0], c.reading[1:] if c.reading else None) for c in self.cands if len(c.text) > 1 and c.char_ends[0] == best.char_ends[0]]
            self.cands = shifted or ([Candidate(best.text[1:], best.score, best.fit_total, [e - n_consumed for e in best.char_ends[1:]], best.prefix + best.text[0], best.reading[1:] if best.reading else None)] if len(best.text) > 1 else [])
    def key(self, k: str) -> List[Candidate]:
        assert len(k) == 1; self.all_keys += k
        self._freeze_until_fits(k); self.window_keys += k
        if len(self.all_keys) < self.decode_from_key:
            self.snapshots.append(Snapshot(self.frozen_keys, self.window_keys, list(self.frozen_ctx), [], 0.0)); self.latencies.append(0.0); return []
        t0 = time.perf_counter(); cands = list(self.decode(Window(keys=self.window_keys, ctx=self._ctx()))); ms = 1000 * (time.perf_counter() - t0)
        for c in cands: c.check(len(self.window_keys), len(self.frozen_ctx))
        self.cands = cands; self.latencies.append(ms)
        self.snapshots.append(Snapshot(self.frozen_keys, self.window_keys, list(self.frozen_ctx), list(cands), ms)); return cands[:self.top]
    def backspace(self) -> None:
        if not self.snapshots: return
        self.snapshots.pop(); self.all_keys = self.all_keys[:-1]; self.n_backspaces += 1
        if self.latencies: self.latencies.pop()
        while self.frozen_positions and self.frozen_positions[-1] > len(self.all_keys): self.frozen_positions.pop()
        if self.snapshots:
            s = self.snapshots[-1]; self.frozen_keys, self.window_keys, self.frozen_ctx, self.cands = s.frozen_keys, s.window_keys, list(s.frozen_ctx), list(s.cands)
        else: self.frozen_keys = 0; self.window_keys = ""; self.frozen_ctx = []; self.cands = []
    def stop(self, gold: str) -> Result:
        top = self.cands[:self.top]; readings = [c.reading for c in top if c.reading is not None]; d = m = None
        if readings: cnt = collections.Counter(readings); d = len(cnt); m = max(cnt.values()) / len(readings)
        return Result(gold=gold, keys=self.all_keys, candidates=[c.full for c in top], frozen_sets=list(self.frozen_ctx), frozen_positions=list(self.frozen_positions), latencies_ms=list(self.latencies), n_backspaces=self.n_backspaces, last_distinct_readings=d, last_max_reading_share=m)

def run_sentence(decode: DecodeFn, keys: str, gold: str, prev_sentence: Optional[str] = None, max_keys: int = 36, edits: Optional[Sequence[str]] = None, decode_from_key: int = 1, set_source: int = 256) -> Result:
    sim = Simulator(decode, max_keys=max_keys, prev_sentence=prev_sentence, decode_from_key=decode_from_key, set_source=set_source); i = 0
    for op in (edits or "k" * len(keys)):
        if op == "B": sim.backspace(); i = max(0, i - 1)
        else: sim.key(keys[i]); i += 1
    assert i == len(keys), f"edit script typed {i} of {len(keys)} keys"
    return sim.stop(gold)
