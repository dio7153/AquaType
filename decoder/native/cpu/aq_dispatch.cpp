// aquadecoder.dll on Windows: the C API, forwarded to the build of the decoder this CPU can run - aquadecoder_avx2.dll
// (AVX2, FMA and F16C, with an OS that saves the YMM registers) or aquadecoder_sse.dll (SSE4.1), from the same folder.
// Both are full decoders built from the same sources; only the instruction set differs, so nothing compiled for AVX2
// can reach a CPU without it.  AQ_ISA=sse picks the SSE build on any machine (to test the fallback).
#include "aquadecoder.h"
#include <windows.h>
#include <intrin.h>
#include <cstdlib>
#include <mutex>
#include <string>
#include <type_traits>

namespace {
struct Api {
    decltype(&aq_open) open; decltype(&aq_close) close; decltype(&aq_decode) decode; decltype(&aq_reset) reset;
    decltype(&aq_maintain) maintain; decltype(&aq_char_id) char_id; decltype(&aq_readings) readings; decltype(&aq_free) free;
    decltype(&aq_last_error) last_error; decltype(&aq_version) version;
};
Api api{}; bool loaded = false; std::string err, version; std::once_flag once;

bool has_avx2() {
    int r[4]; __cpuid(r, 0); if (r[0] < 7) return false;
    __cpuid(r, 1); const int c = r[2];
    if (!((c >> 12) & 1) || !((c >> 27) & 1) || !((c >> 28) & 1) || !((c >> 29) & 1)) return false;   // FMA, OSXSAVE, AVX, F16C
    if ((_xgetbv(0) & 6) != 6) return false;                                                              // the OS saves XMM and YMM
    __cpuidex(r, 7, 0); return (r[1] >> 5) & 1;
}
bool has_sse41() { int r[4]; __cpuid(r, 1); return (r[2] >> 19) & 1; }

std::wstring own_dir() {
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&own_dir, &m);
    wchar_t p[2048]; const DWORD n = GetModuleFileNameW(m, p, 2048); std::wstring s(p, n);
    return s.substr(0, s.find_last_of(L"\\/") + 1);
}

void load() {
    const char* force = std::getenv("AQ_ISA");
    const bool avx2 = has_avx2() && !(force && std::string(force) == "sse");
    if (!avx2 && !has_sse41()) { err = "this CPU has neither AVX2 nor SSE4.1"; return; }
    const char* name = avx2 ? "aquadecoder_avx2.dll" : "aquadecoder_sse.dll";
    const std::wstring path = own_dir() + (avx2 ? L"aquadecoder_avx2.dll" : L"aquadecoder_sse.dll");
    HMODULE h = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!h) { err = std::string("cannot load ") + name + " (error " + std::to_string(GetLastError()) + ")"; return; }
    bool ok = true;
    auto get = [&](auto& f, const char* sym) { f = reinterpret_cast<std::remove_reference_t<decltype(f)>>(GetProcAddress(h, sym)); if (!f) ok = false; };
    get(api.open, "aq_open"); get(api.close, "aq_close"); get(api.decode, "aq_decode"); get(api.reset, "aq_reset");
    get(api.maintain, "aq_maintain"); get(api.char_id, "aq_char_id"); get(api.readings, "aq_readings"); get(api.free, "aq_free");
    get(api.last_error, "aq_last_error"); get(api.version, "aq_version");
    if (!ok) { err = std::string(name) + " lacks part of the C API"; return; }
    version = std::string(api.version()) + (avx2 ? "/avx2" : "/sse");
    loaded = true;
}
bool ready() { std::call_once(once, load); return loaded; }
}  // namespace

extern "C" {
aq_decoder* aq_open(const char* model_dir, const char* metallib_path) { return ready() ? api.open(model_dir, metallib_path) : nullptr; }
void aq_close(aq_decoder* d) { if (d && ready()) api.close(d); }
char* aq_decode(aq_decoder* d, const char* keys, const char* ctx_json) { return ready() ? api.decode(d, keys, ctx_json) : nullptr; }
void aq_reset(aq_decoder* d) { if (ready()) api.reset(d); }
void aq_maintain(aq_decoder* d) { if (ready()) api.maintain(d); }
int aq_char_id(aq_decoder* d, const char* c) { return ready() ? api.char_id(d, c) : -1; }
char* aq_readings(aq_decoder* d, const char* keys) { return ready() ? api.readings(d, keys) : nullptr; }
void aq_free(char* p) { if (p && ready()) api.free(p); }
const char* aq_last_error(void) { return ready() ? api.last_error() : err.c_str(); }
const char* aq_version(void) { return ready() ? version.c_str() : "unloaded"; }
}
