#!/usr/bin/env python3
"""The bundled engine, typed a key at a time through twenty everyday sentences (packaging/check/sentences.tsv, written
for this check, not drawn from any evaluation set): how often the sentence is the first candidate once its keys are
all in, and how long each key's answer takes.  build_app.sh refuses a bundle below the limits given.

  check_engine.py <AquaType.app/Contents/Resources> [--min-top1 N] [--min-top3 N] [--max-p95-ms MS]
"""
import argparse, base64, hashlib, json, os, subprocess, sys, tempfile, time
from pathlib import Path

ap = argparse.ArgumentParser()
ap.add_argument("resources")
ap.add_argument("--min-top1", type=int, default=0)
ap.add_argument("--min-top3", type=int, default=0)
ap.add_argument("--max-p95-ms", type=float, default=1e9)
a = ap.parse_args()
res = Path(a.resources)
rows = [l.rstrip("\n").split("\t") for l in (Path(__file__).parent / "check/sentences.tsv").read_text(encoding="utf-8").splitlines() if l.strip()]
state = tempfile.mkdtemp(prefix="aquatype-check-")
engine = subprocess.Popen(
    [str(res / "AquaTypeEngine"), "--command", "serve-ui-jsonl", "--decoder-dir", str(res / "AquaTypeDecoder"),
     "--metallib", str(res / "lib/mlx.metallib"), "--char-to-keys", str(res / "AquaTypeData/char_to_keys.json"),
     "--state-dir", state, "--fallback-aqds", str(res / "AquaTypeData/dictionary_fallback.v1.aqds"),
     "--fallback-manifest", str(res / "AquaTypeData/dictionary_fallback.v1.aqds.manifest.json")],
    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, bufsize=1)

def ask(raw, generation):
    request = {"schema": "aquatype-ui-fallback-request/v1", "client_id": "check", "client_generation": generation,
               "raw_keys": raw, "raw_identity": base64.b64encode(raw.encode()).decode(), "committed_context_utf8": "",
               "context_identity": hashlib.sha256(b"").hexdigest(), "max_candidates": 63}
    engine.stdin.write(json.dumps(request) + "\n"); engine.stdin.flush()
    return json.loads(engine.stdout.readline())

ask("ji3", 1)                                                     # the first answer loads the model; not timed
generation, times, top1, top3, missed = 1, [], 0, 0, []
for keys, sentence in rows:
    for n in range(1, len(keys) + 1):
        generation += 1
        start = time.perf_counter(); answer = ask(keys[:n], generation); times.append((time.perf_counter() - start) * 1000)
    texts = [c["text_utf8"] for c in answer.get("candidates", [])]
    top1 += texts[:1] == [sentence]; top3 += sentence in texts[:3]
    if sentence not in texts[:3]: missed.append(f"{sentence} -> {texts[:3]}")
engine.stdin.close(); engine.wait()
times.sort()
p50, p95 = times[len(times) // 2], times[int(len(times) * 0.95)]
print(f"{len(rows)} sentences, {len(times)} keys: first {top1}, in the first three {top3}; per key p50 {p50:.0f} ms, p95 {p95:.0f} ms")
for m in missed: print(f"  not in the first three: {m}")
problems = []
if top1 < a.min_top1: problems.append(f"first for {top1}, below {a.min_top1}")
if top3 < a.min_top3: problems.append(f"in the first three for {top3}, below {a.min_top3}")
if p95 > a.max_p95_ms: problems.append(f"p95 {p95:.0f} ms, above {a.max_p95_ms:.0f}")
if problems: print("refusing: " + "; ".join(problems), file=sys.stderr); sys.exit(1)
