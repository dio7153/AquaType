// aq_check: the decoder through its C API, typed a key at a time through packaging/check/sentences.tsv the way the
// engine drives it (decode, then maintain between keys; reset between sentences): how often the sentence is the first
// candidate and in the first three once its keys are all in, and how long each key's decode takes.
//   aq_check <model_dir> <sentences.tsv>            the check
//   aq_check <model_dir> --keys "<key string>"      the candidates for one key string, cold
#include "aquadecoder.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

static std::vector<std::string> texts(const char* js) {   // "text":"..." in order; the decoder writes UTF-8 unescaped
    std::vector<std::string> out; const char* p = js;
    while ((p = std::strstr(p, "\"text\":\"")) != nullptr) { p += 8; std::string s;
        while (*p && *p != '"') { if (*p == '\\' && p[1]) { s += p[1]; p += 2; continue; } s += *p++; }
        out.push_back(s); }
    return out;
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
    std::vector<double> times; int top1 = 0, top3 = 0; std::vector<std::string> missed;
    for (auto& [keys, sentence] : rows) {
        aq_reset(d); std::vector<std::string> last;
        for (size_t n = 1; n <= keys.size(); n++) {
            auto t = std::chrono::steady_clock::now(); char* js = aq_decode(d, keys.substr(0, n).c_str(), nullptr);
            times.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count());
            if (!js) { std::fprintf(stderr, "aq_decode(%s): %s\n", keys.substr(0, n).c_str(), aq_last_error()); return 1; }
            if (n == keys.size()) last = texts(js);
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
    aq_close(d); return 0;
}
