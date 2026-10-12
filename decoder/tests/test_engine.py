"""The single-version engine (Sources/AquaTypeEngine): the app's JSONL contract end to end on the shipped model --
a request returns decoder candidates (memory pinned first after a commit; the model's rows; char_to_keys chars; deduped, all
Chinese), a commit is acknowledged with the memory count, a bad line does not kill the process, and without a decoder the engine
still answers from the traditional path.  Skipped when the engine binary is not built."""
import base64, hashlib, json, os, queue, select, subprocess, sys, threading, time
from pathlib import Path
import pytest
REPO = Path(__file__).resolve().parents[2]; sys.path.insert(0, str(REPO))
# The development build by default; build_app.sh points these at the bundled engine, decoder and dictionary instead.
BIN = Path(os.environ.get("AQUATYPE_TEST_ENGINE", REPO / ".build/debug/AquaTypeEngine"))
AQDS = Path(os.environ.get("AQUATYPE_TEST_AQDS", REPO / "packaging/payload/dict_store/dictionary_fallback.v1.aqds"))
DECODER = Path(os.environ.get("AQUATYPE_TEST_DECODER", REPO / "packaging/payload/decoder"))

def _metallib():
    if os.environ.get("AQUATYPE_TEST_METALLIB"): return os.environ["AQUATYPE_TEST_METALLIB"]
    from decoder.mlx.entry import metallib_path
    return metallib_path()
pytestmark = [pytest.mark.skipif(not BIN.exists(), reason="engine not built (swift build)"),
              pytest.mark.skipif(not AQDS.exists() or not (DECODER / "model.safetensors").exists(),
                                 reason="payload not restored (packaging/restore_payload.sh)")]

def request(raw, gen, ctx="", max_n=9):
    return {"schema": "aquatype-ui-fallback-request/v1", "client_id": "test", "client_generation": gen, "raw_keys": raw, "raw_identity": base64.b64encode(raw.encode()).decode(),
            "committed_context_utf8": ctx, "context_identity": hashlib.sha256(ctx.encode()).hexdigest(), "max_candidates": max_n}
def commit(raw, text, gen, eid, at=None):
    return {"schema": "aquatype-ui-personal-commit/v1", "client_id": "test", "event_id": eid, "client_generation": gen, "raw_keys": raw, "raw_identity": base64.b64encode(raw.encode()).decode(),
            "selected_text": text, "timestamp_unix_seconds": int(time.time() if at is None else at), "count_delta": 1}

class EngineProc:
    def __init__(self, args):
        self.p = subprocess.Popen([str(BIN)] + args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1, encoding="utf-8")
        # select() takes only sockets on Windows: there threads read the pipes, and a reply is waited for on a queue
        self.lines = None
        if os.name == "nt":
            self.lines = queue.Queue(); threading.Thread(target=lambda: [self.lines.put(l) for l in self.p.stdout], daemon=True).start()
            self.err = []; threading.Thread(target=lambda: self.err.extend(self.p.stderr), daemon=True).start()
    def send(self, msg):
        self.p.stdin.write(json.dumps(msg, ensure_ascii=False) + "\n"); self.p.stdin.flush()
        if self.lines is not None:
            try: return json.loads(self.lines.get(timeout=60))
            except queue.Empty: self.p.kill(); raise TimeoutError(f"no answer within 60 s to {msg.get('schema')}")
        if not select.select([self.p.stdout], [], [], 60)[0]:                       # an engine that hangs fails the test
            self.p.kill(); raise TimeoutError(f"no answer within 60 s to {msg.get('schema')}")
        return json.loads(self.p.stdout.readline())
    def raw(self, line): self.p.stdin.write(line + "\n"); self.p.stdin.flush()
    def close(self):
        self.p.stdin.close(); self.p.wait(timeout=30)
        if self.lines is not None: time.sleep(0.1); return "".join(self.err)
        return self.p.stderr.read()

def test_engine_jsonl_contract(tmp_path):
    out = DECODER; state = tmp_path / "state"; state.mkdir(); raw = "5j4u4"                                     # 注意
    from decoder.mlx.entry import metallib_path
    common = ["--fallback-aqds", str(AQDS), "--fallback-manifest", str(AQDS) + ".manifest.json", "--char-to-keys", str(REPO / "data/char_to_keys.json"), "--state-dir", str(state)]
    e = EngineProc(["--command", "serve-ui-jsonl", "--decoder-dir", str(out), "--metallib", _metallib()] + common)
    r = e.send(request(raw, 1)); assert r["visible_path"] == "DECODER_V1" and r["raw_identity"] == base64.b64encode(raw.encode()).decode() and r["candidates"]
    sources = [c["provenance"][0] for c in r["candidates"]]; texts = [c["text_utf8"] for c in r["candidates"]]
    assert "G_DECODER_MODEL" in sources and len(set(texts)) == len(texts) and all(all(0x3400 <= ord(ch) <= 0x9FFF or ord(ch) >= 0x20000 for ch in t) for t in texts)
    ack = e.send(commit(raw, "記甲", 2, "e1")); assert ack["schema"] == "aquatype-ui-personal-commit-ack/v1" and ack["status"] == "RECORDED" and ack["effective_count"] == 1.0
    r2 = e.send(request(raw, 3)); assert r2["candidates"][0]["text_utf8"] == "記甲" and r2["candidates"][0]["provenance"][0] == "G_DECODER_MEMORY"   # first; the model's fusion of it may add its own tag
    e.raw("{not json"); r3 = e.send(request(raw, 4)); assert r3["client_generation"] == 4                           # a bad line is logged, the engine keeps serving
    c2k = json.load(open(REPO / "data/char_to_keys.json", encoding="utf-8")); r4 = e.send(request(c2k["丆"], 5, max_n=9)); assert any(c["text_utf8"] == "丆" and "G_CHAR_TABLE" in c["provenance"] for c in r4["candidates"])   # beyond the decoder cap: the char table is always appended
    err = e.close(); assert "ready decoder=true" in err
    d = EngineProc(["--command", "serve-ui-jsonl", "--decoder-dir", str(tmp_path / "nonexistent")] + common)                 # degraded: no decoder
    r5 = d.send(request(c2k["我"], 1)); assert r5["visible_path"] == "DETERMINISTIC_RIME_AQDS_PERSONAL_V1" and any(c["text_utf8"] == "我" for c in r5["candidates"])
    assert "decoder unavailable" in d.close()

@pytest.mark.skipif(os.name != "nt" or not (REPO / "packaging/payload/rime/terra_pinyin.dict.yaml").exists(), reason="the Windows engine's --char-readings")
def test_other_readings(tmp_path):
    """char_to_keys.json has one reading a character; the dictionary's other readings make 蔚 typable as ㄩˋ too (its table
    reading is ㄨㄟˋ), after the characters that read ㄩˋ in the table; without the dictionary it is not offered."""
    common = ["--command", "serve-ui-jsonl", "--decoder-dir", str(DECODER), "--fallback-aqds", str(AQDS), "--fallback-manifest", str(AQDS) + ".manifest.json",
              "--char-to-keys", str(REPO / "data/char_to_keys.json"), "--state-dir", str(tmp_path)]
    def texts(extra):
        e = EngineProc(common + extra); r = e.send(request("m4", 1, max_n=9)); e.close(); return [c["text_utf8"] for c in r["candidates"]]
    plain, more = texts([]), texts(["--char-readings", str(REPO / "packaging/payload/rime/terra_pinyin.dict.yaml")])
    assert "蔚" not in plain and "蔚" in more and more.index("蔚") > more.index("鬻") and [t for t in more if t in plain] == plain

def test_engine_char_coverage():
    r = subprocess.run([str(BIN), "--check-char-coverage", str(REPO / "data/char_to_keys.json")], capture_output=True, text=True, encoding="utf-8", timeout=300)
    assert r.returncode == 0 and json.loads(r.stdout.strip().splitlines()[-1])["missing"] == 0


def test_third_source_after_a_typo(tmp_path):
    """The first syllable's characters are offered through the single mistakes the decoder corrects, so the model reading
    a typo never leaves Tab with no group to go to: a key too many, two keys swapped, a neighbouring key."""
    state = tmp_path / "state"; state.mkdir()
    from decoder.mlx.entry import metallib_path
    e = EngineProc(["--command", "serve-ui-jsonl", "--decoder-dir", str(DECODER), "--metallib", _metallib(), "--fallback-aqds", str(AQDS),
                    "--fallback-manifest", str(AQDS) + ".manifest.json", "--char-to-keys", str(REPO / "data/char_to_keys.json"), "--state-dir", str(state)])
    cases = {"rupp wu0 ": "今", "rpu wu0 ": "今", "eup wu0 ": "今", "5jj4u4": "注", "j54u4": "注"}   # ㄐㄧㄣ / ㄓㄨˋ typed wrongly
    for gen, (raw, char) in enumerate(cases.items(), 1):
        third = [c["text_utf8"] for c in e.send(request(raw, gen))["candidates"] if c["provenance"][0] in ("G_NATIVE_AQDS", "G_CHAR_TABLE")]
        assert char in third, f"{raw!r}: {char} not among the third source's {third[:8]}"
    e.close()


def test_third_source_follows_the_model_where_the_tones_do_not_cut(tmp_path):
    """Where the tone keys do not cut the input into syllables - a tone left out, so a syllable runs into the next, or a
    syllable with two keys wrong - the third source cuts it the way the model read it: the homophones of its first
    character and the word its first characters spell, each taking exactly the keys the model read them from, so what is
    left after Tab and a choice is the rest of the sentence."""
    state = tmp_path / "state"; state.mkdir()
    from decoder.mlx.entry import metallib_path
    e = EngineProc(["--command", "serve-ui-jsonl", "--decoder-dir", str(DECODER), "--metallib", _metallib(), "--fallback-aqds", str(AQDS),
                    "--fallback-manifest", str(AQDS) + ".manifest.json", "--char-to-keys", str(REPO / "data/char_to_keys.json"), "--state-dir", str(state)])
    # raw keys -> {candidate: the keys it takes}; ㄐㄧㄣ ㄊㄧㄢ ㄨㄛˇ ㄑㄩˋ and ㄓㄨˇ ㄧˋ typed with tones left out or keys wrong.
    # 今天 is the longest word the input starts with, so the word takes only part of the keys.
    cases = {"rupwu0ji3fm4": {"今天": 6, "今": 3},                                                # no tone until the third syllable
             "rupwu0 ji3fm4": {"今天": 7, "今": 3},                                               # the first syllable's tone left out
             "rup wu0ji3fm4": {"今天": 7, "今": 4},                                               # the second's left out: the word only
             "5jj3u4": {"主": 4}}                                                                  # ㄓㄨˇ with a key too many and a wrong tone key
    for gen, (raw, want) in enumerate(cases.items(), 1):
        third = {(c["text_utf8"], c.get("consumed_keys")) for c in e.send(request(raw, gen))["candidates"] if c["provenance"][0] in ("G_NATIVE_AQDS", "G_CHAR_TABLE")}
        for text, keys in want.items():
            assert (text, keys) in third, f"{raw!r}: {text} taking {keys} keys not among the third source's {sorted(third)[:12]}"
    # A first syllable typed right is still cut at its tone key: ㄐㄧㄣ with Space takes its four keys.
    third = {(c["text_utf8"], c.get("consumed_keys")) for c in e.send(request("rup wu0 ji3fm4", 9))["candidates"] if c["provenance"][0] in ("G_NATIVE_AQDS", "G_CHAR_TABLE")}
    assert ("今天", 8) in third and ("今", 4) in third
    e.close()


def test_memory_refuses_text_that_does_not_match_its_keys(tmp_path):
    """Four characters remembered for the two syllables of ㄐㄧㄣ ㄊㄧㄢ - left behind by a commit whose keys and characters
    did not line up - are neither recalled nor fused into the model's 今天, and such a commit is not remembered again."""
    state = tmp_path / "state"; state.mkdir(); now = int(time.time())
    (state / "personal_memory.v2.tsv").write_text(f"噄醫今天\trup wu0 \trup wu0 \t1165,1546\t1.0\t{now}\n", encoding="utf-8")
    from decoder.mlx.entry import metallib_path
    e = EngineProc(["--command", "serve-ui-jsonl", "--decoder-dir", str(DECODER), "--metallib", _metallib(), "--fallback-aqds", str(AQDS),
                    "--fallback-manifest", str(AQDS) + ".manifest.json", "--char-to-keys", str(REPO / "data/char_to_keys.json"), "--state-dir", str(state)])
    texts = lambda r: [c["text_utf8"] for c in r["candidates"]]
    for gen, raw in enumerate(["rup wu0", "rup wu0 "], 1):
        assert not any("噄" in t for t in texts(e.send(request(raw, gen)))), raw
    e.send(commit("rup wu0 ", "甲乙丙丁", 3, "e-mismatch"))
    assert "甲乙丙丁" not in texts(e.send(request("rup wu0 ", 4)))
    err = e.close(); assert "do not match their text" in err and "not remembered" in err

def test_memory_takes_back_a_commit_corrected_at_once(tmp_path):
    """The same keys committed again, differently, within a minute is a correction: the first choice loses the count it
    had just gained (it is kept at nothing, and no longer offered), and the second is remembered.  Both happen in the one write a commit makes.  The memory is
    readable by its owner only."""
    state = tmp_path / "state"; raw = "5j4u4"                                                                      # 注意
    from decoder.mlx.entry import metallib_path
    e = EngineProc(["--command", "serve-ui-jsonl", "--decoder-dir", str(DECODER), "--metallib", _metallib(),
                    "--fallback-aqds", str(AQDS), "--fallback-manifest", str(AQDS) + ".manifest.json",
                    "--char-to-keys", str(REPO / "data/char_to_keys.json"), "--state-dir", str(state)])
    e.send(request(raw, 1)); assert e.send(commit(raw, "注意", 2, "c1"))["status"] == "RECORDED"
    e.send(request(raw, 3)); assert e.send(commit(raw, "助益", 4, "c2"))["status"] == "RECORDED"
    offered = [c["text_utf8"] for c in e.send(request(raw, 5))["candidates"] if "G_DECODER_MEMORY" in c["provenance"]]
    e.close()
    rows = {r[0]: float(r[4]) for r in (l.split("\t") for l in (state / "personal_memory.v2.tsv").read_text(encoding="utf-8").splitlines())}
    assert rows == {"注意": 0.0, "助益": 1.0}, rows          # taken back to nothing, kept for the next time it is chosen
    assert offered == ["助益"], offered
    if os.name == "posix":                                                          # Windows: the profile folder's ACL keeps it private
        assert (state.stat().st_mode & 0o777) == 0o700 and ((state / "personal_memory.v2.tsv").stat().st_mode & 0o777) == 0o600


def test_memory_rewrite_gives_way_to_a_later_choice_of_the_models_characters(tmp_path):
    """A remembered phrase rewrites the model's sentence wherever its sounds appear (記甲 for ㄓㄨˋ ㄧˋ turns 今天注意 into
    今天記甲).  Once the person has chosen the model's own characters for those sounds since, the rewrite stops: an old
    mistaken commit does not keep first place for good.  The other way round - the model's characters remembered first,
    the phrase after (a name corrected later) - the rewrite still applies."""
    from decoder.mlx.entry import metallib_path
    three_years_ago, now = time.time() - 3 * 365 * 86400, time.time()
    def engine(state):
        return EngineProc(["--command", "serve-ui-jsonl", "--decoder-dir", str(DECODER), "--metallib", _metallib(),
                           "--fallback-aqds", str(AQDS), "--fallback-manifest", str(AQDS) + ".manifest.json",
                           "--char-to-keys", str(REPO / "data/char_to_keys.json"), "--state-dir", str(state)])
    # The memories are of 今天注意 and of 記甲; what is typed is a longer sentence with the same sounds in it, so
    # nothing is recalled for exactly its keys and only the rewrite decides the first candidate.
    remembered, typed = "rup wu0 5j4u4", "rup wu0 5j4u4d04"                                                      # 今天注意, 今天注意看
    first = lambda e, gen: e.send(request(typed, gen))["candidates"][0]["text_utf8"]
    e = engine(tmp_path / "a")
    assert first(e, 1) == "今天注意看"                                             # the model's own reading
    e.send(commit("5j4u4", "記甲", 2, "a1", at=three_years_ago))
    assert first(e, 3) == "今天記甲看"                                             # the rewrite is in effect
    e.send(commit(remembered, "今天注意", 4, "a2", at=now))
    assert first(e, 5) == "今天注意看"
    e.close()
    e = engine(tmp_path / "b")
    e.send(commit(remembered, "今天注意", 1, "b1", at=three_years_ago))
    e.send(commit("5j4u4", "記甲", 2, "b2", at=now))
    assert first(e, 3) == "今天記甲看"
    e.close()


def test_memory_keeps_homophones_and_refreshes_what_a_commit_uses(tmp_path):
    """Two single characters typed with the same keys seconds apart (他, then 她) are both remembered: neither is a
    correction of the other.  A memory a later commit contains is as recent as that commit, so a name typed once and
    then only inside sentences does not age out.  A temporary file a killed engine left behind is removed at start."""
    from decoder.mlx.entry import metallib_path
    state = tmp_path / "state"; state.mkdir()
    stale = state / ".personal_memory.v2.tsv.999999.tmp"; stale.write_text("left by an engine that was killed")
    three_years_ago, now = time.time() - 3 * 365 * 86400, time.time()
    e = EngineProc(["--command", "serve-ui-jsonl", "--decoder-dir", str(DECODER), "--metallib", _metallib(),
                    "--fallback-aqds", str(AQDS), "--fallback-manifest", str(AQDS) + ".manifest.json",
                    "--char-to-keys", str(REPO / "data/char_to_keys.json"), "--state-dir", str(state)])
    e.send(commit("w8 ", "他", 1, "h1", at=now)); e.send(commit("w8 ", "她", 2, "h2", at=now + 5))
    e.send(commit("5j4u4", "記甲", 3, "n1", at=three_years_ago))
    e.send(commit("rup wu0 5j4u4d04", "今天記甲看", 4, "n2", at=now))
    e.close()
    rows = {r[0]: r for r in (l.split("\t") for l in (state / "personal_memory.v2.tsv").read_text(encoding="utf-8").splitlines())}
    assert "他" in rows and "她" in rows, sorted(rows)
    assert int(rows["記甲"][5]) >= int(now), rows["記甲"]
    assert not stale.exists()


def test_memory_newer_name_wins_over_older_sentence(tmp_path):
    """Where two remembered rewrites overlap, the better supported wins, not the longer: a name corrected today (謝子萱)
    goes before a sentence from a year ago that had it another way (謝紫軒說明天見)."""
    from decoder.mlx.entry import metallib_path
    state = tmp_path / "state"; state.mkdir()
    a_year_ago, now = time.time() - 365 * 86400, time.time()
    e = EngineProc(["--command", "serve-ui-jsonl", "--decoder-dir", str(DECODER), "--metallib", _metallib(),
                    "--fallback-aqds", str(AQDS), "--fallback-manifest", str(AQDS) + ".manifest.json",
                    "--char-to-keys", str(REPO / "data/char_to_keys.json"), "--state-dir", str(state)])
    sentence = "vu,4y3vm0 gji au/6wu0 ru04"                                                     # ㄒㄧㄝˋ ㄗˇ ㄒㄩㄢ ㄕㄨㄛ ㄇㄧㄥˊ ㄊㄧㄢ ㄐㄧㄢˋ
    e.send(commit(sentence, "謝紫軒說明天見", 1, "s1", at=a_year_ago))
    e.send(commit("vu,4y3vm0 ", "謝子萱", 2, "s2", at=now))
    first = e.send(request(sentence + "d04", 3))["candidates"][0]["text_utf8"]                 # one more syllable: no exact recall
    e.close()
    assert first.startswith("謝子萱"), first


def test_memory_two_engines_never_overwrite_each_other(tmp_path):
    """Two engines writing one memory (an engine started by hand beside the input method's): each sees the other's
    commits before it writes, so none is lost - also now that an unchanged file is not parsed again."""
    from decoder.mlx.entry import metallib_path
    state = tmp_path / "state"; state.mkdir()
    def engine():
        return EngineProc(["--command", "serve-ui-jsonl", "--decoder-dir", str(DECODER), "--metallib", _metallib(),
                           "--fallback-aqds", str(AQDS), "--fallback-manifest", str(AQDS) + ".manifest.json",
                           "--char-to-keys", str(REPO / "data/char_to_keys.json"), "--state-dir", str(state)])
    a, b = engine(), engine()
    a.send(commit("5j4u4", "注意", 1, "a1")); b.send(commit("rup wu0 ", "今天", 1, "b1")); a.send(commit("cl3", "好", 2, "a2"))
    a.close(); b.close()
    texts = {l.split("\t")[0] for l in (state / "personal_memory.v2.tsv").read_text(encoding="utf-8").splitlines()}
    assert texts == {"注意", "今天", "好"}, texts


@pytest.mark.skipif(os.name == "nt", reason="a looping symlink needs the symlink privilege on Windows")
def test_memory_not_replaced_when_it_cannot_be_looked_at(tmp_path):
    """A memory that cannot be looked at for a moment (a network home reconnecting; here a link that loops, which fails
    with ELOOP) is not taken for an empty one: the commit is not written over it."""
    state = tmp_path / "state"; state.mkdir()
    memory = state / "personal_memory.v2.tsv"
    memory.symlink_to(memory.name)
    e = EngineProc(["--command", "serve-ui-jsonl", "--decoder-dir", str(DECODER), "--metallib", _metallib(),
                    "--fallback-aqds", str(AQDS), "--fallback-manifest", str(AQDS) + ".manifest.json",
                    "--char-to-keys", str(REPO / "data/char_to_keys.json"), "--state-dir", str(state)])
    e.send(commit("5j4u4", "注意", 1, "c1"))
    e.close()
    assert memory.is_symlink() and os.readlink(memory) == memory.name
