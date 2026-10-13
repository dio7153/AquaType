#include "aquadecoder.h"
#include "v2_session.h"
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#ifndef AQ_CPU
#include "mlx/backend/metal/metal.h"
#endif
static std::string g_err;
// The C API AquaTypeEngine calls.  An export written by decoder/v2/export_v2.py carries "format": "aquatype-v2/1" in
// its model.json; anything else is refused.
struct aq_decoder { aq::V2Session* v2 = nullptr; };

static bool is_v2_export(const std::string& dir) {
    std::ifstream f(dir + "/model.json"); if (!f) return false;
    std::stringstream ss; ss << f.rdbuf(); const std::string js = ss.str();
    return js.find("aquatype-v2/1") != std::string::npos;
}
static char* dup_js(const std::string& js) { char* out = (char*)std::malloc(js.size() + 1); std::memcpy(out, js.c_str(), js.size() + 1); return out; }
extern "C" {
aq_decoder* aq_open(const char* model_dir, const char* metallib_path) {
    try {
#ifndef AQ_CPU
        if (metallib_path && *metallib_path) mlx::core::metal::set_metallib_path(metallib_path);
#else
        (void)metallib_path;                                                             // no Metal library on the CPU backend
#endif
        const std::string dir = model_dir ? model_dir : "";
        if (!is_v2_export(dir)) { g_err = "not a v2 export: " + dir; return nullptr; }
        return new aq_decoder{new aq::V2Session(dir)}; }
    catch (const std::exception& e) { g_err = e.what(); return nullptr; }
}
void aq_close(aq_decoder* d) { if (d) { delete d->v2; delete d; } }
char* aq_decode(aq_decoder* d, const char* keys, const char* ctx_json) {
    try {
        std::string js = d->v2->decode_json(keys ? keys : "", ctx_json ? ctx_json : "");
        // AQ_LOG_MS: the decoder's own total for this request, so the host's round trip can be split into "the decoder"
        // and "everything else" without guessing.  The engine discards the timing block; this prints it.
        static const bool log_ms = std::getenv("AQ_LOG_MS") != nullptr;
        if (log_ms) {
            auto num = [&](const char* k) { size_t q = js.find(k); return q == std::string::npos ? -1.0 : std::atof(js.c_str() + q + std::strlen(k)); };
            std::fprintf(stderr, "[aq] keys %zu total %.1f syl %.1f lm %.1f beam %.1f spans %.1f prune %.1f steps %.0f%s%s%s\n", std::strlen(keys ? keys : ""),
                         num("\"total\":"), num("\"syllable\":"), num("\"lm\":"), num("\"beam\":"), num("\"spans\":"), num("\"prune\":"), num("\"steps\":"),
                         js.find("\"tier\":\"fast\"") != std::string::npos ? " fast" : "",
                         num("\"resets\":") > 0 ? " overflow-redo" : "", num("\"cache_reset\":") > 0 ? " cache-reset" : "");
        }
        return dup_js(js);
    } catch (const std::exception& e) { g_err = e.what(); return nullptr; }
}
void aq_abort(aq_decoder* d) { if (d) d->v2->abort(); }
void aq_reset(aq_decoder* d) { if (d) d->v2->reset(); }
void aq_maintain(aq_decoder* d) { if (d) d->v2->maintain(); }
int aq_char_id(aq_decoder* d, const char* c) {
    const std::string ch = c ? c : ""; const auto& v = d->v2->chars();
    for (size_t i = 0; i < v.size(); i++) if (v[i] == ch) return (int)i;
    return -1;
}
char* aq_readings(aq_decoder* d, const char* keys) {
    try {
        if (!d || !keys) return dup_js("[]");
        std::vector<int32_t> r = d->v2->exact_readings(keys);
        std::string js = "["; for (size_t i = 0; i < r.size(); i++) { if (i) js += ","; js += std::to_string(r[i]); } js += "]";
        return dup_js(js);
    } catch (const std::exception& e) { g_err = e.what(); return dup_js("[]"); }
}
int aq_tier(aq_decoder* d) { return d && d->v2->tier_fast() ? 1 : 0; }
void aq_set_tier(aq_decoder* d, int fast) { if (d) d->v2->restore_tier(fast != 0); }
void aq_free(char* p) { std::free(p); }
const char* aq_last_error(void) { return g_err.c_str(); }
const char* aq_version(void) { return DECODER_NATIVE_VERSION; }
}
