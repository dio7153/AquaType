// aq_check: the decoder through its C API, typed a key at a time through packaging/check/sentences.tsv the way the
// engine drives it (decode, then maintain between keys; reset between sentences): how often the sentence is the first
// candidate and in the first three once its keys are all in, and how long each key's decode takes.
//   aq_check <model_dir> <sentences.tsv>            the check
//   aq_check <model_dir> <sentences.tsv> --jumps    key-by-key vs several keys at once must give the same candidates
//   aq_check <model_dir> <sentences.tsv> --backspace  backspaces and keys typed back: the forward first candidates
//   aq_check <model_dir> <sentences.tsv> --abort   aq_abort from another thread: stopped decodes, same first candidate
//   aq_check <model_dir> --keys "<key string>"      the candidates for one key string, cold
//   aq_check <model_dir> --tier                     aq_tier / aq_set_tier round trip (run with AQ_TIER unset)
#include "aquadecoder.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#endif

static std::vector<std::string> texts(const char* js) {   // "text":"..." in order; the decoder writes UTF-8 unescaped
    std::vector<std::string> out; const char* p = js;
    while ((p = std::strstr(p, "\"text\":\"")) != nullptr) { p += 8; std::string s;
        while (*p && *p != '"') { if (*p == '\\' && p[1]) { s += p[1]; p += 2; continue; } s += *p++; }
        out.push_back(s); }
    return out;
}

static std::u32string u32(const std::string& s) {           // UTF-8 -> code points (the decoder writes valid UTF-8)
    std::u32string out;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char)s[i]; int n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        char32_t v = n == 1 ? c : n == 2 ? (c & 0x1F) : n == 3 ? (c & 0x0F) : (c & 0x07);
        for (int k = 1; k < n && i + k < s.size(); k++) v = (v << 6) | ((unsigned char)s[i + k] & 0x3F);
        out.push_back(v); i += n;
    }
    return out;
}
static size_t common(const std::u32string& a, const std::u32string& b) { size_t i = 0; while (i < a.size() && i < b.size() && a[i] == b[i]) i++; return i; }

// AQ_SETTLE: how far behind the cursor the first candidate still changes, and what auto-committing would cost.
// "Unsettled" after a key = the first candidate's length minus the part of it that never changes again.  Auto-commit
// with keep D sends out everything but the last D characters of the first candidate after each key; a later key can
// then no longer change those, so the result is the committed part plus the rest of the final candidate.
static void settle(const std::vector<std::vector<std::u32string>>& firsts, const std::vector<std::u32string>& truths) {
    std::vector<int> tail; std::vector<int> worst;
    for (auto& f : firsts) {
        int w = 0; size_t stable = SIZE_MAX;                        // walking back: the prefix shared with every later key
        std::vector<int> t(f.size());
        for (size_t n = f.size(); n-- > 0;) { stable = std::min(stable, common(f[n], f.back())); if (n + 1 < f.size()) stable = std::min(stable, common(f[n], f[n + 1]) ); t[n] = (int)f[n].size() - (int)std::min(stable, f[n].size()); }
        for (int v : t) { tail.push_back(v); w = std::max(w, v); }
        worst.push_back(w);
    }
    std::sort(tail.begin(), tail.end());
    auto pct = [&](double p) { return tail[std::min(tail.size() - 1, (size_t)(tail.size() * p))]; };
    std::printf("unsettled tail (chars) per key: p50 %d p90 %d p99 %d max %d\n", pct(0.5), pct(0.9), pct(0.99), tail.back());
    int base_ok = 0; size_t base_chars = 0, all_chars = 0;
    for (size_t i = 0; i < firsts.size(); i++) { base_ok += firsts[i].back() == truths[i]; base_chars += common(firsts[i].back(), truths[i]); all_chars += truths[i].size(); }
    std::printf("no auto-commit: sentences %d/%zu, characters right up to the first error %.1f%%\n", base_ok, firsts.size(), 100.0 * base_chars / all_chars);
    for (int D : {2, 4, 6, 8, 10, 12, 16}) {
        int ok = 0, changed = 0, commits = 0; size_t chars = 0;
        for (size_t i = 0; i < firsts.size(); i++) {
            std::u32string c;                                       // committed so far
            for (auto& f : firsts[i]) {
                if ((int)f.size() <= D) continue;
                size_t want = f.size() - D;
                if (want > c.size() && common(f, c) >= c.size()) { c = f.substr(0, want); commits++; }
            }
            const auto& fin = firsts[i].back();
            std::u32string res = c + (fin.size() > c.size() ? fin.substr(std::min(c.size(), fin.size())) : std::u32string());
            changed += res != fin; ok += res == truths[i]; chars += common(res, truths[i]);
        }
        std::printf("keep %2d: sentences %d/%zu, characters right up to the first error %.1f%%, differs from no-commit %d, commits %d\n",
                    D, ok, firsts.size(), 100.0 * chars / all_chars, changed, commits);
    }
    std::printf("worst unsettled tail per sentence:"); for (int w : worst) std::printf(" %d", w); std::printf("\n");
}

static std::vector<int> first_ends(const char* js) {          // the first candidate's "char_ends":[...]
    std::vector<int> out; const char* p = std::strstr(js, "\"char_ends\":["); if (!p) return out; p += 13;
    while (*p && *p != ']') { out.push_back(std::atoi(p)); while (*p && *p != ',' && *p != ']') p++; if (*p == ',') p++; }
    return out;
}
static std::string utf8(const std::u32string& s) {
    std::string o;
    for (char32_t c : s) {
        if (c < 0x80) o += (char)c;
        else if (c < 0x800) { o += (char)(0xC0 | (c >> 6)); o += (char)(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { o += (char)(0xE0 | (c >> 12)); o += (char)(0x80 | ((c >> 6) & 0x3F)); o += (char)(0x80 | (c & 0x3F)); }
        else { o += (char)(0xF0 | (c >> 18)); o += (char)(0x80 | ((c >> 12) & 0x3F)); o += (char)(0x80 | ((c >> 6) & 0x3F)); o += (char)(0x80 | (c & 0x3F)); }
    }
    return o;
}

// AQ_COMMIT=D: the auto-commit itself, as the host would run it.  After each key, when the first candidate is longer than
// D characters, its first len-D characters are committed: their keys (up to char_ends) leave the key string and the
// characters join the context (kind 0, the last 64, as the engine passes the previous sentence).  The result is the
// committed text plus the first candidate of the last decode.
// AQ_COMMIT_K=K: a character is committed only once the first candidate has kept it (and everything before it) for K keys
// in a row; K = 0 is the plain length rule.
static int commit_run(aq_decoder* d, const std::vector<std::pair<std::string, std::string>>& rows, int D) {
    const int K = std::getenv("AQ_COMMIT_K") ? std::atoi(std::getenv("AQ_COMMIT_K")) : 0;
    std::vector<double> times; int ok = 0, commits = 0, at_cut = 0, wrong = 0;
    for (auto& [keys, sentence] : rows) {
        aq_reset(d); std::u32string committed, top, prev_full; std::vector<size_t> cuts; std::vector<int> run; size_t off = 0;
        for (size_t n = 1; n <= keys.size(); n++) {
            if (n <= off) continue;
            std::string ctx = "[";
            for (size_t i = committed.size() > 64 ? committed.size() - 64 : 0; i < committed.size(); i++) {
                if (ctx.size() > 1) ctx += ",";
                ctx += "{\"kind\":0,\"chars\":[" + std::to_string(aq_char_id(d, utf8(committed.substr(i, 1)).c_str())) + "],\"weights\":[1.0]}";
            }
            ctx += "]";
            auto t = std::chrono::steady_clock::now();
            char* js = aq_decode(d, keys.substr(off, n - off).c_str(), committed.empty() ? nullptr : ctx.c_str());
            times.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count());
            if (!js) { std::fprintf(stderr, "aq_decode: %s\n", aq_last_error()); return 1; }
            auto tx = texts(js); auto ends = first_ends(js); aq_free(js); aq_maintain(d);
            top = tx.empty() ? std::u32string() : u32(tx[0]);
            // stability: how many keys in a row each character of committed+top has stood unchanged (with all before it)
            const std::u32string full = committed + top; const size_t cp = common(full, prev_full);
            run.resize(full.size()); for (size_t i = 0; i < full.size(); i++) run[i] = i < cp ? run[i] + 1 : 0;
            prev_full = full;
            size_t k = (int)top.size() > D ? top.size() - D : 0;
            while (k > 0 && run[committed.size() + k - 1] < K) k--;                 // only characters that have settled
            for (size_t i = 0; i < k; i++) if (run[committed.size() + i] < K) { k = i; break; }
            if (k > 0 && n < keys.size() && ends.size() == top.size()) {
                size_t used = (size_t)ends[k - 1] + 1;
                committed += top.substr(0, k); off += used; cuts.push_back(committed.size()); commits++; top.clear();
                if (std::getenv("AQ_COMMIT_DEBUG")) std::printf("  commit %s | keys used %zu, left \"%s\"\n", utf8(committed).c_str(), used, keys.substr(off, n - off).c_str());
            }
        }
        const std::u32string res = committed + top, truth = u32(sentence);
        const bool good = res == truth; ok += good;
        if (!good) { wrong++; size_t e = common(res, truth); for (size_t c : cuts) if (e + 1 >= c && e <= c + 1) { at_cut++; break; } }
        std::printf("%s %s -> %s\n", good ? "1" : "x", sentence.c_str(), utf8(res).c_str());
    }
    std::sort(times.begin(), times.end());
    std::printf("auto-commit keep %d K %d: %d/%zu sentences, %d commits, %d of %d misses have their first error next to a commit point; per key p50 %.0f ms, p95 %.0f ms, max %.0f ms\n",
                D, K, ok, rows.size(), commits, at_cut, wrong, times[times.size() / 2], times[(size_t)(times.size() * 0.95)], times.back());
    return 0;
}

// --jumps: the host decodes only the newest keys when it falls behind (the text service keeps one request in flight and
// sends whatever was typed meanwhile), so a decode often extends the previous keys by several at once.  Each sentence
// is typed twice, a key at a time and in jumps of 1-5 keys.  The frozen window keeps the beam of whichever keystroke
// came before, so near-ties further down can come out in another order; the first candidate after the last key must
// match (on packaging/check/sentences.tsv it does for every sentence), and the frozen window must have been used on the
// jumps (frozen_positions > 0), or the check would pass on cold decodes.  Exit status 1 otherwise.  On long inputs the
// first candidate can differ too; how often each way is right is printed.
static double num_after(const char* js, const char* key) { const char* p = std::strstr(js, key); return p ? std::atof(p + std::strlen(key)) : -1; }
static int jumps_run(aq_decoder* d, const std::vector<std::pair<std::string, std::string>>& rows) {
    unsigned seed = 7; auto rnd = [&] { seed = seed * 1103515245u + 12345u; return (seed >> 16) % 5 + 1; };
    int same = 0, top1_same = 0, frozen_jumps = 0, jumps = 0, right_a = 0, right_b = 0;
    for (auto& [keys, sentence] : rows) {
        std::vector<std::string> a, b;
        aq_reset(d);
        for (size_t n = 1; n <= keys.size(); n++) { char* js = aq_decode(d, keys.substr(0, n).c_str(), nullptr); if (!js) return 2; if (n == keys.size()) a = texts(js); aq_free(js); aq_maintain(d); }
        aq_reset(d);
        for (size_t n = 0; n < keys.size();) {
            const size_t step = rnd(); const bool jump = step > 1 && n > 0; n = std::min(keys.size(), n + step);
            char* js = aq_decode(d, keys.substr(0, n).c_str(), nullptr); if (!js) return 2;
            if (jump) { jumps++; frozen_jumps += num_after(js, "\"frozen_positions\":") > 0; }
            if (n == keys.size()) b = texts(js);
            aq_free(js); aq_maintain(d);
        }
        a.resize(std::min<size_t>(3, a.size())); b.resize(std::min<size_t>(3, b.size()));
        const bool eq = a == b; same += eq; top1_same += !a.empty() && !b.empty() && a[0] == b[0];
        right_a += !a.empty() && a[0] == sentence; right_b += !b.empty() && b[0] == sentence;
        if (!eq) std::printf("differs: %s\n  key by key: %s / %s / %s\n  in jumps:   %s / %s / %s\n", sentence.c_str(),
                             a.size() > 0 ? a[0].c_str() : "", a.size() > 1 ? a[1].c_str() : "", a.size() > 2 ? a[2].c_str() : "",
                             b.size() > 0 ? b[0].c_str() : "", b.size() > 1 ? b[1].c_str() : "", b.size() > 2 ? b[2].c_str() : "");
    }
    std::printf("jumps: %d/%zu sentences with the same first three (first candidate %d/%zu); %d of %d multi-key decodes reused the frozen window; first candidate right: key by key %d, in jumps %d\n",
                same, rows.size(), top1_same, rows.size(), frozen_jumps, jumps, right_a, right_b);
    return top1_same == (int)rows.size() && frozen_jumps > 0 ? 0 : 1;
}

// AQ_SEGMENTS: per-key decode times by how long the composition is (characters in the first candidate after the key),
// in the bands of the release latency targets: <=20 chars p95 100 ms (p99 150), 21-40 p95 130, 41-60 p95 150.  The
// first sentence after opening (cold caches) is also shown on its own.  The decoder only: the pipe and the text
// service add their share on top.
static void segments(const std::vector<double>& times, const std::vector<std::vector<std::u32string>>& firsts) {
    struct Band { const char* name; int lo, hi; double p95, p99; std::vector<double> t; };
    std::vector<Band> bands = {{"1-20", 1, 20, 100, 150, {}}, {"21-40", 21, 40, 130, 0, {}}, {"41-60", 41, 60, 150, 0, {}}, {"61+", 61, 1 << 30, 0, 0, {}}};
    std::vector<double> cold; size_t k = 0;
    for (size_t s = 0; s < firsts.size(); s++) for (auto& f : firsts[s]) {
        const int n = std::max(1, (int)f.size()); const double t = times[k++];
        if (s == 0) cold.push_back(t);
        for (auto& b : bands) if (n >= b.lo && n <= b.hi) b.t.push_back(t);
    }
    auto pct = [](std::vector<double> v, double p) { std::sort(v.begin(), v.end()); return v.empty() ? 0.0 : v[std::min(v.size() - 1, (size_t)(v.size() * p))]; };
    for (auto& b : bands) {
        if (b.t.empty()) continue;
        const double p95 = pct(b.t, 0.95), p99 = pct(b.t, 0.99);
        std::printf("band %-6s keys %5zu  p50 %4.0f  p95 %4.0f  p99 %4.0f  max %5.0f ms", b.name, b.t.size(), pct(b.t, 0.5), p95, p99, pct(b.t, 1.0));
        if (b.p95 > 0) std::printf("  target p95 %.0f%s", b.p95, p95 <= b.p95 ? " ok" : " MISS");
        if (b.p99 > 0) std::printf(", p99 %.0f%s", b.p99, p99 <= b.p99 ? " ok" : " MISS");
        std::printf("\n");
    }
    if (!cold.empty()) std::printf("first sentence (cold): keys %zu  p50 %.0f  max %.0f ms\n", cold.size(), pct(cold, 0.5), pct(cold, 1.0));
}

// --abort: aq_abort from another thread, the way the engine stops a stale decode when newer keys arrive.  In every run of
// AQ_ABORT_RUN keystrokes (default 3) all but the last are aborted after 0-40 ms, as on a machine slower than the typing;
// then the sentence is typed again skipping exactly the keystrokes that did abort (what the host would have sent had it
// known).  A stopped decode hands on the beam positions it completed, so near-ties further down can come out in another
// order, as with --jumps; the first candidate after the last key must match.  Exit status 1 on a different first
// candidate or if nothing aborted.  On long inputs with long runs the first candidate can differ too; how often each way
// is right is printed.
static int abort_run(aq_decoder* d, const std::vector<std::pair<std::string, std::string>>& rows) {
    unsigned seed = 11; auto rnd = [&](unsigned m) { seed = seed * 1103515245u + 12345u; return (seed >> 16) % m; };
    const int run = std::max(2, std::getenv("AQ_ABORT_RUN") ? std::atoi(std::getenv("AQ_ABORT_RUN")) : 3);
    int same = 0, top1_same = 0, aborted = 0, tried = 0, right_a = 0, right_b = 0;
    for (auto& [keys, sentence] : rows) {
        std::vector<char> skipped(keys.size() + 1, 0); std::vector<std::string> a, b;
        aq_reset(d);
        for (size_t n = 1; n <= keys.size(); n++) {
            const bool stop = n % run != 0 && n < keys.size(); std::thread th;
            if (stop) { const unsigned wait = rnd(41); tried++; th = std::thread([d, wait] { std::this_thread::sleep_for(std::chrono::milliseconds(wait)); aq_abort(d); }); }
            char* js = aq_decode(d, keys.substr(0, n).c_str(), nullptr);
            if (th.joinable()) th.join();
            if (!js) return 2;
            if (std::strstr(js, "\"aborted\":true")) { skipped[n] = 1; aborted++; }
            else { if (n == keys.size()) a = texts(js); aq_maintain(d); }
            aq_free(js);
        }
        aq_reset(d);
        for (size_t n = 1; n <= keys.size(); n++) {
            if (skipped[n]) continue;
            char* js = aq_decode(d, keys.substr(0, n).c_str(), nullptr); if (!js) return 2;
            if (n == keys.size()) b = texts(js);
            aq_free(js); aq_maintain(d);
        }
        a.resize(std::min<size_t>(3, a.size())); b.resize(std::min<size_t>(3, b.size()));
        same += a == b; top1_same += !a.empty() && !b.empty() && a[0] == b[0];
        right_a += !a.empty() && a[0] == sentence; right_b += !b.empty() && b[0] == sentence;
        if (a != b) std::printf("differs: %s\n  with aborts: %s\n  skipping:    %s\n", sentence.c_str(), a.empty() ? "" : a[0].c_str(), b.empty() ? "" : b[0].c_str());
    }
    std::printf("abort: %d of %d attempted aborts stopped a decode; %d/%zu sentences with the same first three as typing without those keys (first candidate %d/%zu); first candidate right: with aborts %d, skipping %d\n",
                aborted, tried, same, rows.size(), top1_same, rows.size(), right_a, right_b);
    return top1_same == (int)rows.size() && aborted > 0 ? 0 : 1;
}

// --backspace: each sentence typed a key at a time, then one backspace, the key typed back, three backspaces and those
// keys typed back.  Every one of those four decodes reuses the beam below its frozen line, so the first candidate must
// be the one the same keys had on the way forward.  Exit status 1 on a different first candidate; the backspace decodes'
// times are printed (a cold decode of a long composition is a second or more).
static int backspace_run(aq_decoder* d, const std::vector<std::pair<std::string, std::string>>& rows) {
    int top1_same = 0, top3_same = 0, cmp = 0, right_fwd = 0, right_back = 0; std::vector<double> ms;
    auto top3 = [](std::vector<std::string> v) { v.resize(std::min<size_t>(3, v.size())); return v; };
    for (auto& [keys, sentence] : rows) {
        if (keys.size() < 4) continue;
        const size_t n = keys.size(); std::vector<std::string> fwd1, fwd3, fwd;
        aq_reset(d);
        for (size_t k = 1; k <= n; k++) { char* js = aq_decode(d, keys.substr(0, k).c_str(), nullptr); if (!js) return 2;
            if (k == n - 3) fwd3 = top3(texts(js)); if (k == n - 1) fwd1 = top3(texts(js)); if (k == n) fwd = top3(texts(js)); aq_free(js); aq_maintain(d); }
        const std::pair<size_t, const std::vector<std::string>*> steps[] = {{n - 1, &fwd1}, {n, &fwd}, {n - 3, &fwd3}, {n, &fwd}};
        std::vector<std::string> last;
        for (int s = 0; s < 4; s++) {
            auto t = std::chrono::steady_clock::now(); char* js = aq_decode(d, keys.substr(0, steps[s].first).c_str(), nullptr);
            const double t_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count(); if (!js) return 2;
            if (s == 0 || s == 2) ms.push_back(t_ms);
            const std::vector<std::string> got = top3(texts(js)); aq_free(js); aq_maintain(d); last = got;
            const auto& want = *steps[s].second; cmp++;
            top3_same += got == want; top1_same += !got.empty() && !want.empty() && got[0] == want[0];
            if (got.empty() || want.empty() || got[0] != want[0]) std::printf("differs (%s, %zu keys): %s\n  forward:   %s\n  backspace: %s\n", s % 2 ? "typed back" : "backspace", steps[s].first,
                                                                    sentence.c_str(), want.empty() ? "" : want[0].c_str(), got.empty() ? "" : got[0].c_str());
        }
        right_fwd += !fwd.empty() && fwd[0] == sentence; right_back += !last.empty() && last[0] == sentence;
    }
    std::sort(ms.begin(), ms.end()); auto pct = [&](double p) { return ms.empty() ? 0.0 : ms[std::min(ms.size() - 1, (size_t)(ms.size() * p))]; };
    std::printf("backspace: %d/%d decodes with the same first candidate as on the way forward (first three %d); backspace decode p50 %.0f p95 %.0f max %.0f ms; first candidate right: forward %d, after the backspaces %d\n",
                top1_same, cmp, top3_same, pct(0.5), pct(0.95), pct(1.0), right_fwd, right_back);
    return top1_same == cmp ? 0 : 1;
}

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    if (argc < 3) { std::fprintf(stderr, "usage: aq_check <model_dir> <sentences.tsv> | --keys <keys>\n"); return 2; }
    auto t0 = std::chrono::steady_clock::now();
    aq_decoder* d = aq_open(argv[1], nullptr);
    if (!d) { std::fprintf(stderr, "aq_open: %s\n", aq_last_error()); return 1; }
    std::printf("decoder %s, loaded in %.0f ms\n", aq_version(), std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    if (std::strcmp(argv[2], "--tier") == 0) {             // aq_set_tier reaches the decode (auto mode: AQ_TIER unset)
        const int t0 = aq_tier(d); aq_set_tier(d, 1); char* js = aq_decode(d, "ji3", nullptr);
        const bool fast = js && std::strstr(js, "\"tier\":\"fast\""); aq_free(js);
        const int t1 = aq_tier(d); aq_set_tier(d, 0); const int t2 = aq_tier(d);
        std::printf("tier: opened %d, after aq_set_tier(1) %d (decode says %s), after aq_set_tier(0) %d\n", t0, t1, fast ? "fast" : "accurate", t2);
        aq_close(d); return t0 == 0 && t1 == 1 && fast && t2 == 0 ? 0 : 1;
    }
    if (std::strcmp(argv[2], "--keys") == 0 && argc > 3) {
        for (int rep = 0; rep < 2; rep++) { aq_reset(d); auto t = std::chrono::steady_clock::now(); char* js = aq_decode(d, argv[3], nullptr);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
            if (!js) { std::fprintf(stderr, "aq_decode: %s\n", aq_last_error()); return 1; }
            auto tx = texts(js); std::printf("cold decode %.1f ms, %zu candidates\n", ms, tx.size());
            if (rep == 1) for (size_t i = 0; i < tx.size() && i < 10; i++) std::printf("  %zu. %s\n", i + 1, tx[i].c_str());
            aq_free(js); }
        aq_close(d); return 0;
    }
    std::ifstream f(argv[2]); if (!f) { std::fprintf(stderr, "cannot read %s\n", argv[2]); return 1; }
    std::vector<std::pair<std::string, std::string>> rows; std::string line;
    while (std::getline(f, line)) { if (!line.empty() && line.back() == '\r') line.pop_back(); size_t tab = line.find('\t'); if (tab == std::string::npos) continue; rows.push_back({line.substr(0, tab), line.substr(tab + 1)}); }
    // AQ_REPEAT=N: the rows N times over (a long session); AQ_MEM: the process's memory every 100 sentences, to see that
    // it levels off instead of growing with use
    if (const char* r = std::getenv("AQ_REPEAT")) { const auto one = rows; for (int i = 1; i < std::atoi(r); i++) rows.insert(rows.end(), one.begin(), one.end()); }
    const bool mem = std::getenv("AQ_MEM") != nullptr;
    auto print_mem = [&](size_t done) {
#ifdef _WIN32
        PROCESS_MEMORY_COUNTERS_EX pm{}; GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pm, sizeof(pm));
        std::fprintf(stderr, "mem sentences %zu working %.0f MB private %.0f MB peak working %.0f MB\n", done, pm.WorkingSetSize / 1048576.0, pm.PrivateUsage / 1048576.0, pm.PeakWorkingSetSize / 1048576.0);
#else
        (void)done;
#endif
    };
    { char* js = aq_decode(d, "ji3", nullptr); aq_free(js); aq_reset(d); }       // the first decode warms up; not timed
    if (mem) print_mem(0);
    if (argc > 3 && std::strcmp(argv[3], "--jumps") == 0) { int r = jumps_run(d, rows); aq_close(d); return r; }
    if (argc > 3 && std::strcmp(argv[3], "--abort") == 0) { int r = abort_run(d, rows); aq_close(d); return r; }
    if (argc > 3 && std::strcmp(argv[3], "--backspace") == 0) { int r = backspace_run(d, rows); aq_close(d); return r; }
    if (const char* ac = std::getenv("AQ_COMMIT")) { int r = commit_run(d, rows, std::atoi(ac)); aq_close(d); return r; }
    std::vector<double> times; int top1 = 0, top3 = 0; std::vector<std::string> missed;
    std::vector<std::vector<std::u32string>> firsts;              // per sentence, the first candidate after each key
    std::vector<std::u32string> truths;
    for (auto& [keys, sentence] : rows) {
        aq_reset(d); std::vector<std::string> last; firsts.emplace_back(); truths.push_back(u32(sentence));
        for (size_t n = 1; n <= keys.size(); n++) {
            auto t = std::chrono::steady_clock::now(); char* js = aq_decode(d, keys.substr(0, n).c_str(), nullptr);
            times.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count());
            if (!js) { std::fprintf(stderr, "aq_decode(%s): %s\n", keys.substr(0, n).c_str(), aq_last_error()); return 1; }
            auto tx = texts(js); firsts.back().push_back(tx.empty() ? std::u32string() : u32(tx[0]));
            if (n == keys.size()) last = tx;
            aq_free(js); aq_maintain(d);
        }
        const bool t1 = !last.empty() && last[0] == sentence, t3 = std::find(last.begin(), last.begin() + std::min<size_t>(3, last.size()), sentence) != last.begin() + std::min<size_t>(3, last.size());
        top1 += t1; top3 += t3;
        std::string got; for (size_t i = 0; i < last.size() && i < 3; i++) { if (i) got += " / "; got += last[i]; }
        std::printf("%s %-24s -> %s\n", t1 ? "1" : t3 ? "3" : "x", sentence.c_str(), got.c_str());
        if (mem && truths.size() % 100 == 0) print_mem(truths.size());
    }
    std::vector<double> s = times; std::sort(s.begin(), s.end());
    std::printf("%zu sentences, %zu keys: first %d, in the first three %d; per key p50 %.0f ms, p95 %.0f ms, max %.0f ms\n",
                rows.size(), s.size(), top1, top3, s[s.size() / 2], s[(size_t)(s.size() * 0.95)], s.back());
    if (std::getenv("AQ_SETTLE")) settle(firsts, truths);
    if (std::getenv("AQ_SEGMENTS")) segments(times, firsts);
    aq_close(d); return 0;
}
