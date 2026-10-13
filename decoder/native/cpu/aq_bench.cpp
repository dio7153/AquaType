// aq_bench: can this machine run the decoder fast enough?  For the installer, before the input method is set up: it loads
// aquadecoder.dll from its own folder (dynamically, so a missing DLL or an unsupported CPU is an exit code, not a
// loader dialog), types a fixed set of key strings a key at a time the way the engine drives the decoder (warm-up
// first, decode then maintain, reset between sentences), and prints one JSON line:
//   {"version":"...","isa":"avx2|sse","cores":N,"keys":K,"seconds":S,"p50":..,"p95":..,"p99":..,"max":..,"overhead":O,"verdict":"ok|discuss|block"}
// The percentiles are the decoder's own per-key times plus --overhead (the pipe and the text service, measured end to
// end on a known machine).  Exit status: 0 ok (p95 <= --ok), 1 discuss (p95 <= --block), 2 block (p95 > --block),
// 3 the decoder does not load or decode (CPU without SSE4.1, files missing).
//   aq_bench <model_dir> [--ok 150] [--block 300] [--overhead 0] [--budget 40]
// --budget: seconds; a slow machine stops early and is judged on the keys it got through (at least 60, or "block").
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

struct aq_decoder;
typedef aq_decoder* (*open_fn)(const char*, const char*);
typedef void (*close_fn)(aq_decoder*);
typedef char* (*decode_fn)(aq_decoder*, const char*, const char*);
typedef void (*void_fn)(aq_decoder*);
typedef void (*free_fn)(char*);
typedef const char* (*str_fn)(void);

// packaging/check/sentences.tsv's key strings (the first ten), then three of them run together: short sentences and a
// 25-35 character composition, the lengths people type most.
static const char* kShort[] = {
    "rup wu0 wu0 fu4cp3cl3", "ji3ap7au/6wu0 vu84j3d9 cjo4", "5k4ek4jp4wu6vm ul4y94wl3xjp4", "j03g;4ul41j4ul4u fu3t z04",
    "fu/31831l4el4ru4eo3ji3", "w8 u3ru/ 2l4ej/ n xk7", "5. ai4vu;3fm4c931u0 y.3y.3", "5k41p3gj vu,32k6cp3u.3u4n ",
    "ru42k6294m3n03tj ap6", "vu84h4ru04au04y94xul6187"};

static int fail(const char* what, const char* detail) {
    std::string msg = std::string(what) + (detail && *detail ? std::string(": ") + detail : std::string()), esc;
    for (char c : msg) { if (c == '\\' || c == '"') esc += '\\'; if ((unsigned char)c >= 0x20) esc += c; }   // paths carry backslashes
    std::printf("{\"verdict\":\"error\",\"error\":\"%s\"}\n", esc.c_str());
    return 3;
}

int main(int argc, char** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);              // no dialog if a DLL is missing
    if (argc < 2) { std::fprintf(stderr, "usage: aq_bench <model_dir> [--ok ms] [--block ms] [--overhead ms] [--budget s]\n"); return 3; }
    double ok_ms = 150, block_ms = 300, overhead = 0, budget = 40;
    for (int i = 2; i + 1 < argc; i += 2) {
        const double v = std::atof(argv[i + 1]);
        if (!std::strcmp(argv[i], "--ok")) ok_ms = v; else if (!std::strcmp(argv[i], "--block")) block_ms = v;
        else if (!std::strcmp(argv[i], "--overhead")) overhead = v; else if (!std::strcmp(argv[i], "--budget")) budget = v;
    }
    wchar_t self[MAX_PATH]; const DWORD n = GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring dir(self, n); dir = dir.substr(0, dir.find_last_of(L"\\/") + 1);
    HMODULE h = LoadLibraryExW((dir + L"aquadecoder.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!h) return fail("cannot load aquadecoder.dll", std::to_string(GetLastError()).c_str());
    auto aq_open = (open_fn)GetProcAddress(h, "aq_open"); auto aq_close = (close_fn)GetProcAddress(h, "aq_close");
    auto aq_decode = (decode_fn)GetProcAddress(h, "aq_decode"); auto aq_reset = (void_fn)GetProcAddress(h, "aq_reset");
    auto aq_maintain = (void_fn)GetProcAddress(h, "aq_maintain"); auto aq_free = (free_fn)GetProcAddress(h, "aq_free");
    auto aq_last_error = (str_fn)GetProcAddress(h, "aq_last_error"); auto aq_version = (str_fn)GetProcAddress(h, "aq_version");
    if (!aq_open || !aq_close || !aq_decode || !aq_reset || !aq_maintain || !aq_free || !aq_last_error || !aq_version) return fail("aquadecoder.dll is missing exports", "");
    aq_decoder* d = aq_open(argv[1], nullptr);
    if (!d) return fail("aq_open", aq_last_error());          // also: a CPU with neither AVX2 nor SSE4.1
    const std::string version = aq_version();
    const std::string isa = version.find("/avx2") != std::string::npos ? "avx2" : version.find("/sse") != std::string::npos ? "sse" : "?";

    std::vector<std::string> work(std::begin(kShort), std::end(kShort));
    work.push_back(std::string(kShort[0]) + kShort[1] + kShort[2]); work.push_back(std::string(kShort[4]) + kShort[5] + kShort[6]);
    work.push_back(std::string(kShort[7]) + kShort[8] + kShort[9]);
    { char* js = aq_decode(d, "ji3", nullptr); if (!js) return fail("aq_decode", aq_last_error()); aq_free(js); aq_reset(d); }   // the engine's warm-up
    using clk = std::chrono::steady_clock; const auto t0 = clk::now();
    std::vector<double> ms; bool stopped = false;
    for (auto& keys : work) {
        aq_reset(d);
        for (size_t k = 1; k <= keys.size(); k++) {
            const auto t = clk::now(); char* js = aq_decode(d, keys.substr(0, k).c_str(), nullptr);
            ms.push_back(std::chrono::duration<double, std::milli>(clk::now() - t).count() + overhead);
            if (!js) return fail("aq_decode", aq_last_error());
            aq_free(js); aq_maintain(d);
            if (std::chrono::duration<double>(clk::now() - t0).count() > budget) { stopped = true; break; }
        }
        if (stopped) break;
    }
    const double secs = std::chrono::duration<double>(clk::now() - t0).count();
    aq_close(d);
    std::vector<double> s = ms; std::sort(s.begin(), s.end());
    auto pct = [&](double p) { return s.empty() ? 0.0 : s[std::min(s.size() - 1, (size_t)(s.size() * p))]; };
    const double p95 = pct(0.95);
    // too slow to get through 60 keys inside the budget is a block whatever the percentiles say
    const int code = s.size() < 60 ? 2 : p95 <= ok_ms ? 0 : p95 <= block_ms ? 1 : 2;
    const char* verdict = code == 0 ? "ok" : code == 1 ? "discuss" : "block";
    std::printf("{\"version\":\"%s\",\"isa\":\"%s\",\"cores\":%u,\"keys\":%zu,\"seconds\":%.1f,\"p50\":%.0f,\"p95\":%.0f,\"p99\":%.0f,\"max\":%.0f,\"overhead\":%.0f,\"stopped_early\":%s,\"verdict\":\"%s\"}\n",
                version.c_str(), isa.c_str(), std::thread::hardware_concurrency(), s.size(), secs, pct(0.5), p95, pct(0.99), pct(1.0), overhead, stopped ? "true" : "false", verdict);
    std::fflush(stdout);
    return code;
}
