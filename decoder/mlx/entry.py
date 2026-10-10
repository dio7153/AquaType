"""P6.2: the deployment entry from Python -- the C++ DecodeSession (native enumerate / prune / assemble + MLX model) wrapped as
the frozen scorer's `decode(Window) -> list[Candidate]`, so the P3 evaluation and the product share one entry.
    NativeDecoder(model_dir).decode(window)   # model_dir from decoder/mlx/export.py
"""
from __future__ import annotations
import json, os, sys
from pathlib import Path
from typing import List, Optional
REPO = Path(__file__).resolve().parents[2]; sys.path.insert(0, str(REPO))
from decoder.core.types import Window
from decoder.eval.simulator import Candidate

def metallib_path() -> str:
    for p in sys.path:
        q = Path(p) / "mlx" / "lib" / "mlx.metallib"
        if q.exists(): return str(q)
    return ""

class NativeDecoder:
    def __init__(self, model_dir: Path, metallib: Optional[str] = None):
        from decoder import native as N
        assert N.AVAILABLE and getattr(N.decoder_native, "WITH_MLX", False), "build with decoder/native/build.sh --mlx"
        self.model_dir = str(model_dir); self.chars = json.loads((Path(model_dir) / "tables.json").read_text(encoding="utf-8"))["chars"]; self.cid = {c: i for i, c in enumerate(self.chars)}
        self.session = N.decoder_native.DecodeSession(self.model_dir, metallib if metallib is not None else metallib_path()); self.last_timing = None
    def ctx_json(self, window: Window) -> str:
        toks = [{"kind": 0 if t.kind == "prev_sentence" else 1, "chars": [self.cid.get(c, -1) for c in t.chars], "weights": list(t.weights)} for t in window.ctx]
        return json.dumps(toks) if toks else ""
    def decode(self, window: Window) -> List[Candidate]:
        rep = json.loads(self.session.decode_json(window.keys, self.ctx_json(window))); self.last_timing = rep.get("timing_ms"); self.last_counts = rep.get("counts")
        return [Candidate(text=c["text"], score=float(c["score"]), fit_total=int(c["fit_total"]), char_ends=list(c["char_ends"]), prefix=c.get("prefix", ""), reading=tuple(c["reading"])) for c in rep["candidates"]]
