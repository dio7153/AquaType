// aq_check: the decoder through its C API, typed a key at a time through packaging/check/sentences.tsv the way the
// engine drives it (decode, then maintain between keys; reset between sentences): how often the sentence is the first
// candidate and in the first three once its keys are all in, and how long each key's decode takes.
//   aq_check <model_dir> <sentences.tsv>            the check
//   aq_check <model_dir> --keys "<key string>"      the candidates for one key string, cold
#include "aquadecoder.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
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

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    if (argc < 3) { std::fprintf(stderr, "usage: aq_check <model_dir> <sentences.tsv> | --keys <keys>\n"); return 2; }
    auto t0 = std::chrono::steady_clock::now();
    aq_decoder* d = aq_open(argv[1], nullptr);
    if (!d) { std::fprintf(stderr, "aq_open: %s\n", aq_last_error()); return 1; }
    std::printf("decoder %s, loaded in %.0f ms\n", aq_version(), std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
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
    { char* js = aq_decode(d, "ji3", nullptr); aq_free(js); aq_reset(d); }       // the first decode warms up; not timed
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
    }
    std::vector<double> s = times; std::sort(s.begin(), s.end());
    std::printf("%zu sentences, %zu keys: first %d, in the first three %d; per key p50 %.0f ms, p95 %.0f ms, max %.0f ms\n",
                rows.size(), s.size(), top1, top3, s[s.size() / 2], s[(size_t)(s.size() * 0.95)], s.back());
    if (std::getenv("AQ_SETTLE")) settle(firsts, truths);
    aq_close(d); return 0;
}
