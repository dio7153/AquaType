"""Core API objects of the v1 research core. Compact numpy arrays, no hidden state."""
from __future__ import annotations
from dataclasses import dataclass, field
from typing import List, Optional, Tuple
import numpy as np

FIT_DIMS = 5             # per candidate: [substitute, insert, delete, transpose, channel cost x FIT_CHANNEL_SCALE]
FIT_COUNT_DIMS = 4       # the op-count dims: zero-edit test and lambda x edits use these only (spec fit_feature.zero_edit_uses_count_dims_only)
FIT_CHANNEL_SCALE = 100  # int16 fixed point of the channel cost (spec fit_feature.channel_likelihood.fixed_point_scale)

def fit_edits(fit):
    """edit count of a fit vector / array: sum of the op-count dims only"""
    return fit[..., :FIT_COUNT_DIMS].sum(-1)

@dataclass
class CtxToken:
    kind: str                 # "prev_sentence" | "frozen"
    chars: List[str]
    weights: List[float]

@dataclass
class Window:
    keys: str
    ctx: List[CtxToken] = field(default_factory=list)

@dataclass
class Lattice:
    n_keys: int
    seg_start: np.ndarray     # int32[S]
    seg_end: np.ndarray       # int32[S]
    seg_offset: np.ndarray    # int32[S+1]
    cand_seg: np.ndarray      # int32[A]
    cand_syl: np.ndarray      # int32[A]
    cand_fit: np.ndarray      # int16[A,FIT_DIMS]: 4 op counts + channel cost (fixed point)
    _native: object = field(default=None, repr=False, compare=False)   # decoder_native.Lattice handle (index cached), optional
    def n_segments(self) -> int: return int(self.seg_start.shape[0])
    def n_candidates(self) -> int: return int(self.cand_syl.shape[0])
    def check(self, max_edits: int = 3, max_edits_long: int = 4, long_seg_min: int = 5) -> None:
        S, A = self.n_segments(), self.n_candidates()
        assert self.seg_offset.shape == (S + 1,) and self.seg_offset[0] == 0 and self.seg_offset[-1] == A
        assert np.all(self.seg_start <= self.seg_end) and np.all(self.seg_end < self.n_keys)
        seg_len = self.seg_end - self.seg_start + 1; allowed = np.where(seg_len >= long_seg_min, max_edits_long, max_edits)[self.cand_seg]
        assert self.cand_fit.shape == (A, FIT_DIMS) and np.all(fit_edits(self.cand_fit) <= allowed)
        assert np.all(self.cand_fit[:, FIT_COUNT_DIMS] >= 0) and np.all(self.cand_fit[fit_edits(self.cand_fit) == 0, FIT_COUNT_DIMS] == 0), "an exact match has channel cost 0"   # hand-made test tables may leave the channel dim 0 elsewhere
        for s in range(S):
            a, b = self.seg_offset[s], self.seg_offset[s + 1]
            assert np.all(self.cand_seg[a:b] == s), "cand_seg inconsistent with seg_offset"
            assert len(set(self.cand_syl[a:b].tolist())) == b - a, "duplicate syllable within a segment"

@dataclass
class Scores:
    cand_logp: np.ndarray     # float32[A]
    seg_exist: np.ndarray     # float32[S]

@dataclass
class Pruned:
    node_end: np.ndarray      # int32[N]
    node_syl: np.ndarray      # int32[N]
    node_arc_offset: np.ndarray  # int32[N+1]
    node_arcs: np.ndarray     # int32[M] candidate indices into the Lattice
    node_gap: np.ndarray      # float32[N]
    gold_survived: Optional[bool] = None
    def n_nodes(self) -> int: return int(self.node_end.shape[0])

@dataclass
class ZScores:
    node_score: np.ndarray            # float32[Z,N]
    node_best_char: np.ndarray        # int32[Z,N]
    node_best_char_logp: np.ndarray   # float32[Z,N]

@dataclass
class Assembly:
    sent_nodes: List[np.ndarray]      # each int32[·] node indices in order
    sent_chars: List[str]
    sent_score: np.ndarray            # float32[K]
    sent_z: np.ndarray                # int32[K]
    def __len__(self) -> int: return len(self.sent_chars)

Ranked = List[Tuple[str, float, int]]   # (text, score, fit_total)
