#include "cpu_ops.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <thread>
#include <immintrin.h>
#include <omp.h>

namespace aq {
namespace cpu {

size_t dtype_size(Dtype t) { switch (t) { case Dtype::f32: case Dtype::u32: case Dtype::i32: return 4; case Dtype::f16: return 2; default: return 1; } }
array::array(Dtype t, std::vector<int> shape) : dtype_(t), shape_(std::move(shape)) { buf_ = std::make_shared<std::vector<uint8_t>>(size() * dtype_size(t)); }
array::array(Dtype t, std::vector<int> shape, std::shared_ptr<std::vector<uint8_t>> buf, size_t offset) : dtype_(t), shape_(std::move(shape)), buf_(std::move(buf)), off_(offset) {}

// ---------------------------------------------------------------- safetensors
// 8-byte little-endian header length, a JSON header {"name": {"dtype": "F16", "shape": [...], "data_offsets": [a, b]}, ...},
// then the data.  The headers are written by safetensors itself: flat, no escapes in the names this reader needs.
std::pair<std::map<std::string, array>, std::map<std::string, std::string>> load_safetensors(const std::string& path) {
    std::ifstream f(path, std::ios::binary); if (!f) throw std::runtime_error("cannot read " + path);
    f.seekg(0, std::ios::end); const size_t total = (size_t)f.tellg(); f.seekg(0);
    uint64_t hlen = 0; f.read((char*)&hlen, 8); if (hlen > total) throw std::runtime_error("bad safetensors header: " + path);
    std::string hdr(hlen, '\0'); f.read(&hdr[0], (std::streamsize)hlen);
    auto buf = std::make_shared<std::vector<uint8_t>>(total - 8 - hlen);
    f.read((char*)buf->data(), (std::streamsize)buf->size()); if (!f) throw std::runtime_error("short read: " + path);
    std::map<std::string, array> out;
    size_t p = hdr.find('{') + 1;
    while (true) {
        size_t q = hdr.find('"', p); if (q == std::string::npos) break;
        size_t qe = hdr.find('"', q + 1); std::string name = hdr.substr(q + 1, qe - q - 1);
        size_t v = hdr.find(':', qe) + 1; while (v < hdr.size() && hdr[v] == ' ') v++;
        if (hdr[v] != '{') { p = hdr.find_first_of(",}", v); if (p == std::string::npos) break; p++; continue; }   // a scalar value (e.g. "__metadata__": null)
        size_t ob = v, cb = hdr.find('}', ob); std::string body = hdr.substr(ob, cb - ob + 1); p = cb + 1;
        if (name == "__metadata__") continue;
        auto field = [&](const char* k) { size_t a = body.find(std::string("\"") + k + "\""); if (a == std::string::npos) throw std::runtime_error("safetensors: no " + std::string(k) + " for " + name); return a + std::strlen(k) + 2; };
        size_t d0 = body.find('"', body.find(':', field("dtype")) + 1), d1 = body.find('"', d0 + 1); std::string dt = body.substr(d0 + 1, d1 - d0 - 1);
        auto nums = [&](size_t at) { std::vector<long long> v; size_t a = body.find('[', at), e = body.find(']', a); std::string s = body.substr(a + 1, e - a - 1); size_t i = 0;
            while (i < s.size()) { while (i < s.size() && (s[i] == ' ' || s[i] == ',')) i++; if (i >= s.size()) break; v.push_back(std::atoll(s.c_str() + i)); while (i < s.size() && s[i] != ',') i++; } return v; };
        std::vector<long long> sh = nums(field("shape")), off = nums(field("data_offsets"));
        Dtype t; if (dt == "F32") t = Dtype::f32; else if (dt == "F16") t = Dtype::f16; else if (dt == "U32") t = Dtype::u32; else if (dt == "I32") t = Dtype::i32; else if (dt == "U8") t = Dtype::u8; else if (dt == "I8") t = Dtype::i8;
        else throw std::runtime_error("safetensors: unsupported dtype " + dt + " for " + name);
        std::vector<int> shape(sh.begin(), sh.end());
        array a(t, shape, buf, (size_t)off.at(0));
        if ((size_t)(off.at(1) - off.at(0)) != a.size() * dtype_size(t) || (size_t)off.at(1) > buf->size()) throw std::runtime_error("safetensors: bad extent for " + name);
        out.emplace(name, a);
    }
    return {out, {}};
}

// ---------------------------------------------------------------- fp16
float h2f(uint16_t h) { return _mm_cvtss_f32(_mm_cvtph_ps(_mm_cvtsi32_si128(h))); }
uint16_t f2h(float f) { return (uint16_t)_mm_cvtsi128_si32(_mm_cvtps_ph(_mm_set_ss(f), _MM_FROUND_TO_NEAREST_INT)); }
void h2f_n(const uint16_t* s, float* d, size_t n) { size_t i = 0; for (; i + 8 <= n; i += 8) _mm256_storeu_ps(d + i, _mm256_cvtph_ps(_mm_loadu_si128((const __m128i*)(s + i)))); for (; i < n; i++) d[i] = h2f(s[i]); }
void f2h_n(const float* s, uint16_t* d, size_t n) { size_t i = 0; for (; i + 8 <= n; i += 8) _mm_storeu_si128((__m128i*)(d + i), _mm256_cvtps_ph(_mm256_loadu_ps(s + i), _MM_FROUND_TO_NEAREST_INT)); for (; i < n; i++) d[i] = f2h(s[i]); }
std::vector<float> to_f32(const array& a) {
    std::vector<float> v(a.size());
    if (a.dtype() == Dtype::f32) std::memcpy(v.data(), a.data<float>(), v.size() * 4);
    else if (a.dtype() == Dtype::f16) h2f_n(a.data<uint16_t>(), v.data(), v.size());
    else throw std::runtime_error("to_f32: not a float tensor");
    return v;
}

void QWeight::dequant_row(int r, float* dst) const {
    const int per = 32 / bits, words = in / per, ng = in / group; const uint32_t mask = (1u << bits) - 1;
    const uint32_t* row = q + (size_t)r * words; const float* sc = scales.data() + (size_t)r * ng; const float* bi = biases.data() + (size_t)r * ng;
    if (bits == 4 && group % 8 == 0) {
        const __m256i shifts = _mm256_setr_epi32(0, 4, 8, 12, 16, 20, 24, 28), m4 = _mm256_set1_epi32(0xF);
        for (int w = 0; w < words; w++) {
            const int g = (w * 8) / group; __m256i v = _mm256_and_si256(_mm256_srlv_epi32(_mm256_set1_epi32((int)row[w]), shifts), m4);
            _mm256_storeu_ps(dst + w * 8, _mm256_fmadd_ps(_mm256_cvtepi32_ps(v), _mm256_set1_ps(sc[g]), _mm256_set1_ps(bi[g])));
        }
        return;
    }
    for (int i = 0; i < in; i++) { uint32_t v = (row[i / per] >> ((i % per) * bits)) & mask; dst[i] = (float)v * sc[i / group] + bi[i / group]; }
}

// ---------------------------------------------------------------- GEMM
static inline float hsum(__m256 v) { __m128 a = _mm_add_ps(_mm256_castps256_ps128(v), _mm256_extractf128_ps(v, 1)); a = _mm_add_ps(a, _mm_movehl_ps(a, a)); a = _mm_add_ss(a, _mm_shuffle_ps(a, a, 1)); return _mm_cvtss_f32(a); }

// R rows of x against C rows of w, the whole K: R*C dot products, one register each
template <int R, int C>
static inline void micro(const float* x, int K, const float* w, float* y, int ldy, const float* bias, bool acc) {
    __m256 s[R][C];
    for (int r = 0; r < R; r++) for (int c = 0; c < C; c++) s[r][c] = _mm256_setzero_ps();
    int k = 0;
    for (; k + 8 <= K; k += 8) {
        __m256 wv[C]; for (int c = 0; c < C; c++) wv[c] = _mm256_loadu_ps(w + (size_t)c * K + k);
        for (int r = 0; r < R; r++) { __m256 xv = _mm256_loadu_ps(x + (size_t)r * K + k); for (int c = 0; c < C; c++) s[r][c] = _mm256_fmadd_ps(xv, wv[c], s[r][c]); }
    }
    for (int r = 0; r < R; r++) for (int c = 0; c < C; c++) {
        float t = hsum(s[r][c]); for (int kk = k; kk < K; kk++) t += x[(size_t)r * K + kk] * w[(size_t)c * K + kk];
        if (bias) t += bias[c]; float& o = y[(size_t)r * ldy + c]; o = acc ? o + t : t;
    }
}
// y[n, m] block: rows [i0, i1) of x, rows [j0, j1) of w (w points at row 0), y row stride ldy
static void block(const float* x, int K, const float* w, int i0, int i1, int j0, int j1, float* y, int ldy, const float* bias, bool acc) {
    int i = i0;
    for (; i + 4 <= i1; i += 4) { int j = j0; for (; j + 2 <= j1; j += 2) micro<4, 2>(x + (size_t)i * K, K, w + (size_t)j * K, y + (size_t)i * ldy + j, ldy, bias ? bias + j : nullptr, acc);
        for (; j < j1; j++) micro<4, 1>(x + (size_t)i * K, K, w + (size_t)j * K, y + (size_t)i * ldy + j, ldy, bias ? bias + j : nullptr, acc); }
    for (; i < i1; i++) { int j = j0; for (; j + 4 <= j1; j += 4) micro<1, 4>(x + (size_t)i * K, K, w + (size_t)j * K, y + (size_t)i * ldy + j, ldy, bias ? bias + j : nullptr, acc);
        for (; j < j1; j++) micro<1, 1>(x + (size_t)i * K, K, w + (size_t)j * K, y + (size_t)i * ldy + j, ldy, bias ? bias + j : nullptr, acc); }
}

int num_threads() {
    static const int n = [] { const char* e = std::getenv("AQ_THREADS"); if (e && std::atoi(e) > 0) return std::atoi(e);
        int hw = (int)std::thread::hardware_concurrency(); return std::max(1, std::min(8, hw / 2)); }();   // physical cores, at most 8: an input method shares the machine
    return n;
}

void gemm_nt(const float* x, int n, int K, const float* w, int m, float* y, const float* bias, bool acc) {
    if (n <= 0 || m <= 0) return;
    const int NT = 32, MT = 64, tn = (n + NT - 1) / NT, tm = (m + MT - 1) / MT, tiles = tn * tm;
    const long long work = (long long)n * m * K;
    if (work < 200000 || tiles == 1) { block(x, K, w, 0, n, 0, m, y, m, bias, acc); return; }
    #pragma omp parallel for schedule(dynamic) num_threads(num_threads())
    for (int t = 0; t < tiles; t++) { const int bi = t / tm, bj = t % tm;
        block(x, K, w, bi * NT, std::min(n, (bi + 1) * NT), bj * MT, std::min(m, (bj + 1) * MT), y, m, bias, acc); }
}

// ---- packed GEMM: the weight in panels of NR output columns, k-major inside a panel, so the kernel broadcasts one x value
// and multiplies it into two full vectors of weights: MR x NR outputs in 2 * MR registers, two loads per MR * 2 FMAs.
static constexpr int NR = 16, MR = 6, RB = 48;   // RB: x rows per tile (a multiple of MR)
Packed pack_nt(const float* w, int m, int K) {
    Packed P; P.m = m; P.K = K; const int np = (m + NR - 1) / NR; P.p.assign((size_t)np * K * NR, 0.0f);
    for (int j = 0; j < m; j++) { float* dst = P.p.data() + (size_t)(j / NR) * K * NR + (j % NR); const float* src = w + (size_t)j * K;
        for (int k = 0; k < K; k++) dst[(size_t)k * NR] = src[k]; }
    return P;
}
// 6 rows of x (row stride K) x one panel, every accumulator a named register: MSVC spills arrays of __m256
static inline void mk6(const float* x, int K, const float* panel, float* y, int ldy, int nr, int nc, const float* bias, bool acc) {
    __m256 a0 = _mm256_setzero_ps(), a1 = a0, a2 = a0, a3 = a0, a4 = a0, a5 = a0, b0 = a0, b1 = a0, b2 = a0, b3 = a0, b4 = a0, b5 = a0;
    const float *x0 = x, *x1 = x + K, *x2 = x + 2 * (size_t)K, *x3 = x + 3 * (size_t)K, *x4 = x + 4 * (size_t)K, *x5 = x + 5 * (size_t)K;
    for (int k = 0; k < K; k++) {
        const __m256 w0 = _mm256_loadu_ps(panel + (size_t)k * NR), w1 = _mm256_loadu_ps(panel + (size_t)k * NR + 8);
        __m256 v = _mm256_broadcast_ss(x0 + k); a0 = _mm256_fmadd_ps(v, w0, a0); b0 = _mm256_fmadd_ps(v, w1, b0);
        v = _mm256_broadcast_ss(x1 + k); a1 = _mm256_fmadd_ps(v, w0, a1); b1 = _mm256_fmadd_ps(v, w1, b1);
        v = _mm256_broadcast_ss(x2 + k); a2 = _mm256_fmadd_ps(v, w0, a2); b2 = _mm256_fmadd_ps(v, w1, b2);
        v = _mm256_broadcast_ss(x3 + k); a3 = _mm256_fmadd_ps(v, w0, a3); b3 = _mm256_fmadd_ps(v, w1, b3);
        v = _mm256_broadcast_ss(x4 + k); a4 = _mm256_fmadd_ps(v, w0, a4); b4 = _mm256_fmadd_ps(v, w1, b4);
        v = _mm256_broadcast_ss(x5 + k); a5 = _mm256_fmadd_ps(v, w0, a5); b5 = _mm256_fmadd_ps(v, w1, b5);
    }
    alignas(32) float t[MR][NR];
    _mm256_store_ps(t[0], a0); _mm256_store_ps(t[0] + 8, b0); _mm256_store_ps(t[1], a1); _mm256_store_ps(t[1] + 8, b1);
    _mm256_store_ps(t[2], a2); _mm256_store_ps(t[2] + 8, b2); _mm256_store_ps(t[3], a3); _mm256_store_ps(t[3] + 8, b3);
    _mm256_store_ps(t[4], a4); _mm256_store_ps(t[4] + 8, b4); _mm256_store_ps(t[5], a5); _mm256_store_ps(t[5] + 8, b5);
    for (int r = 0; r < nr; r++) { float* o = y + (size_t)r * ldy;
        for (int j = 0; j < nc; j++) { const float v = t[r][j] + (bias ? bias[j] : 0.0f); o[j] = acc ? o[j] + v : v; } }
}
// rows [i0, i1) of x against one panel (columns j0 .. j0 + nc); a short last block runs on a zero-padded copy
static void panel_rows(const float* x, int K, const float* panel, int i0, int i1, int j0, int nc, float* y, int ldy, const float* bias, bool acc) {
    int i = i0;
    for (; i + MR <= i1; i += MR) mk6(x + (size_t)i * K, K, panel, y + (size_t)i * ldy + j0, ldy, MR, nc, bias, acc);
    if (i < i1) { thread_local std::vector<float> pad; pad.assign((size_t)MR * K, 0.0f); std::memcpy(pad.data(), x + (size_t)i * K, (size_t)(i1 - i) * K * sizeof(float));
        mk6(pad.data(), K, panel, y + (size_t)i * ldy + j0, ldy, i1 - i, nc, bias, acc); }
}
void gemm_packed(const float* x, int n, const Packed& W, float* y, const float* bias, bool acc) {
    const int m = W.m, K = W.K; if (n <= 0 || m <= 0) return;
    const int np = (m + NR - 1) / NR, nrb = (n + RB - 1) / RB, tiles = np * nrb;
    auto tile = [&](int t) { const int p = t / nrb, rb = t % nrb, j0 = p * NR;
        panel_rows(x, K, W.p.data() + (size_t)p * K * NR, rb * RB, std::min(n, (rb + 1) * RB), j0, std::min(NR, m - j0), y, m, bias ? bias + j0 : nullptr, acc); };
    if ((long long)n * m * K < 400000) { for (int t = 0; t < tiles; t++) tile(t); return; }
    #pragma omp parallel for schedule(static) num_threads(num_threads())
    for (int t = 0; t < tiles; t++) tile(t);
}

// the quantised weight: each thread dequantises a panel of NR rows straight into the packed layout, then runs every x row
void gemm_q(const float* x, int n, const QWeight& W, float* y, const float* bias, const std::vector<int32_t>* rows) {
    const int m = rows ? (int)rows->size() : W.out, K = W.in; if (n <= 0 || m <= 0) return;
    const int np = (m + NR - 1) / NR;
    auto run = [&](int p, std::vector<float>& row, float* panel) {
        const int j0 = p * NR, nc = std::min(NR, m - j0);
        for (int j = 0; j < NR; j++) {
            if (j < nc) { W.dequant_row(rows ? (*rows)[j0 + j] : j0 + j, row.data()); for (int k = 0; k < K; k++) panel[(size_t)k * NR + j] = row[k]; }
            else for (int k = 0; k < K; k++) panel[(size_t)k * NR + j] = 0.0f; }
        float bsel[NR]; if (bias) for (int j = 0; j < nc; j++) bsel[j] = bias[rows ? (*rows)[j0 + j] : j0 + j];
        panel_rows(x, K, panel, 0, n, j0, nc, y, m, bias ? bsel : nullptr, false);
    };
    #pragma omp parallel num_threads(num_threads()) if (np > 1)
    { std::vector<float> row(K); float* panel = (float*)_mm_malloc((size_t)K * NR * sizeof(float), 32);
      #pragma omp for schedule(dynamic)
      for (int p = 0; p < np; p++) run(p, row, panel);
      _mm_free(panel); }
}

// ---------------------------------------------------------------- elementwise
static void layer_norm_row(const float* xr, int d, const float* g, const float* b, float eps, float* yr) {
    int c = 0; __m256 s8 = _mm256_setzero_ps();
    for (; c + 8 <= d; c += 8) s8 = _mm256_add_ps(s8, _mm256_loadu_ps(xr + c));
    float s = hsum(s8); for (; c < d; c++) s += xr[c]; const float mean = s / d;
    const __m256 m8 = _mm256_set1_ps(mean); __m256 q8 = _mm256_setzero_ps(); c = 0;
    for (; c + 8 <= d; c += 8) { const __m256 t = _mm256_sub_ps(_mm256_loadu_ps(xr + c), m8); q8 = _mm256_fmadd_ps(t, t, q8); }
    float q = hsum(q8); for (; c < d; c++) { const float t = xr[c] - mean; q += t * t; }
    const float rstd = 1.0f / std::sqrt(q / d + eps); const __m256 r8 = _mm256_set1_ps(rstd); c = 0;
    for (; c + 8 <= d; c += 8) _mm256_storeu_ps(yr + c, _mm256_fmadd_ps(_mm256_mul_ps(_mm256_sub_ps(_mm256_loadu_ps(xr + c), m8), r8), _mm256_loadu_ps(g + c), _mm256_loadu_ps(b + c)));
    for (; c < d; c++) yr[c] = (xr[c] - mean) * rstd * g[c] + b[c];
}
void layer_norm(const float* x, int n, int d, const float* g, const float* b, float eps, float* y) {
    #pragma omp parallel for num_threads(num_threads()) if ((long long)n * d > 65536)
    for (int r = 0; r < n; r++) layer_norm_row(x + (size_t)r * d, d, g, b, eps, y + (size_t)r * d);
}
float quant_i8(const float* src, int n, int8_t* dst) {
    int c = 0; __m256 mx8 = _mm256_setzero_ps(); const __m256 sign = _mm256_set1_ps(-0.0f);
    for (; c + 8 <= n; c += 8) mx8 = _mm256_max_ps(mx8, _mm256_andnot_ps(sign, _mm256_loadu_ps(src + c)));
    alignas(32) float t[8]; _mm256_store_ps(t, mx8); float mx = 0; for (float v : t) mx = std::max(mx, v); for (; c < n; c++) mx = std::max(mx, std::fabs(src[c]));
    const float s = std::max(mx / 127.0f, 1e-6f), inv = 1.0f / s; const __m256 i8 = _mm256_set1_ps(inv); c = 0;
    alignas(32) int32_t q[8];
    for (; c + 8 <= n; c += 8) { _mm256_store_si256((__m256i*)q, _mm256_cvtps_epi32(_mm256_mul_ps(_mm256_loadu_ps(src + c), i8))); for (int j = 0; j < 8; j++) dst[c + j] = (int8_t)q[j]; }   // round to nearest even, as rint
    for (; c < n; c++) dst[c] = (int8_t)std::nearbyint(src[c] * inv);
    return s;
}
static inline __m256 i8x8(const int8_t* p) { return _mm256_cvtepi32_ps(_mm256_cvtepi8_epi32(_mm_loadl_epi64((const __m128i*)p))); }
float dot_f32_i8(const float* q, const int8_t* k, int n) {
    int c = 0; __m256 s = _mm256_setzero_ps(); for (; c + 8 <= n; c += 8) s = _mm256_fmadd_ps(_mm256_loadu_ps(q + c), i8x8(k + c), s);
    float r = hsum(s); for (; c < n; c++) r += q[c] * (float)k[c]; return r;
}
void scale_add_i8(float* acc, float cf, float w, const int8_t* v, int n) {
    int c = 0; const __m256 c8 = _mm256_set1_ps(cf), w8 = _mm256_set1_ps(w);
    for (; c + 8 <= n; c += 8) _mm256_storeu_ps(acc + c, _mm256_fmadd_ps(w8, i8x8(v + c), _mm256_mul_ps(_mm256_loadu_ps(acc + c), c8)));
    for (; c < n; c++) acc[c] = acc[c] * cf + w * (float)v[c];
}
// exp on 8 lanes (Cephes polynomial; |rel err| ~1e-7 over the float range)
static inline __m256 exp8(__m256 x) {
    x = _mm256_min_ps(x, _mm256_set1_ps(88.3762626647949f)); x = _mm256_max_ps(x, _mm256_set1_ps(-88.3762626647949f));
    __m256 fx = _mm256_floor_ps(_mm256_fmadd_ps(x, _mm256_set1_ps(1.44269504088896341f), _mm256_set1_ps(0.5f)));
    x = _mm256_fnmadd_ps(fx, _mm256_set1_ps(0.693359375f), x); x = _mm256_fnmadd_ps(fx, _mm256_set1_ps(-2.12194440e-4f), x);
    __m256 y = _mm256_set1_ps(1.9875691500E-4f);
    y = _mm256_fmadd_ps(y, x, _mm256_set1_ps(1.3981999507E-3f)); y = _mm256_fmadd_ps(y, x, _mm256_set1_ps(8.3334519073E-3f));
    y = _mm256_fmadd_ps(y, x, _mm256_set1_ps(4.1665795894E-2f)); y = _mm256_fmadd_ps(y, x, _mm256_set1_ps(1.6666665459E-1f));
    y = _mm256_fmadd_ps(y, x, _mm256_set1_ps(5.0000001201E-1f)); y = _mm256_fmadd_ps(y, _mm256_mul_ps(x, x), _mm256_add_ps(x, _mm256_set1_ps(1.0f)));
    __m256i e = _mm256_slli_epi32(_mm256_add_epi32(_mm256_cvttps_epi32(fx), _mm256_set1_epi32(127)), 23);
    return _mm256_mul_ps(y, _mm256_castsi256_ps(e));
}
// MLX's erf (mlx/backend/metal/kernels/erf.h), both branches on every lane and a blend
static inline __m256 erf8(__m256 a) {
    const __m256 t = _mm256_andnot_ps(_mm256_set1_ps(-0.0f), a), s = _mm256_mul_ps(a, a);
    __m256 r = _mm256_fmadd_ps(_mm256_set1_ps(-1.72853470e-5f), t, _mm256_set1_ps(3.83197126e-4f));
    __m256 u = _mm256_fmadd_ps(_mm256_set1_ps(-3.88396438e-3f), t, _mm256_set1_ps(2.42546219e-2f));
    r = _mm256_fmadd_ps(r, s, u); r = _mm256_fmadd_ps(r, t, _mm256_set1_ps(-1.06777877e-1f)); r = _mm256_fmadd_ps(r, t, _mm256_set1_ps(-6.34846687e-1f));
    r = _mm256_fmadd_ps(r, t, _mm256_set1_ps(-1.28717512e-1f)); r = _mm256_fmadd_ps(r, t, _mm256_sub_ps(_mm256_setzero_ps(), t));
    r = _mm256_sub_ps(_mm256_set1_ps(1.0f), exp8(r));                                              // -(exp(r) - 1)
    r = _mm256_or_ps(r, _mm256_and_ps(a, _mm256_set1_ps(-0.0f)));                                  // copysign (r >= 0 here)
    __m256 p = _mm256_set1_ps(-5.96761703e-4f);
    p = _mm256_fmadd_ps(p, s, _mm256_set1_ps(4.99119423e-3f)); p = _mm256_fmadd_ps(p, s, _mm256_set1_ps(-2.67681349e-2f));
    p = _mm256_fmadd_ps(p, s, _mm256_set1_ps(1.12819925e-1f)); p = _mm256_fmadd_ps(p, s, _mm256_set1_ps(-3.76125336e-1f));
    p = _mm256_fmadd_ps(p, s, _mm256_set1_ps(1.28379166e-1f)); p = _mm256_fmadd_ps(p, a, a);
    return _mm256_blendv_ps(p, r, _mm256_cmp_ps(t, _mm256_set1_ps(0.927734375f), _CMP_GT_OQ));
}
void gelu_inplace(float* x, size_t n) {
    auto body = [](float* p, size_t k) { size_t i = 0;
        for (; i + 8 <= k; i += 8) { __m256 v = _mm256_loadu_ps(p + i); _mm256_storeu_ps(p + i, _mm256_mul_ps(_mm256_mul_ps(_mm256_set1_ps(0.5f), v), _mm256_add_ps(_mm256_set1_ps(1.0f), erf8(_mm256_mul_ps(v, _mm256_set1_ps(0.70710678118654752f)))))); }
        for (; i < k; i++) p[i] = 0.5f * p[i] * (1.0f + std::erf(p[i] * 0.70710678118654752f)); };
    if (n < 65536) { body(x, n); return; }
    const int chunks = num_threads() * 4; const size_t per = ((n / chunks) + 7) & ~(size_t)7;
    #pragma omp parallel for num_threads(num_threads())
    for (int c = 0; c < chunks; c++) { const size_t a = (size_t)c * per; if (a < n) body(x + a, std::min(per, n - a)); }
}
float logsumexp_row(const float* x, int n) {
    float mx = -INFINITY; for (int i = 0; i < n; i++) mx = std::max(mx, x[i]);
    if (!std::isfinite(mx)) return mx;
    __m256 acc = _mm256_setzero_ps(), m = _mm256_set1_ps(mx); int i = 0;
    for (; i + 8 <= n; i += 8) acc = _mm256_add_ps(acc, exp8(_mm256_sub_ps(_mm256_loadu_ps(x + i), m)));
    float s = hsum(acc); for (; i < n; i++) s += std::exp(x[i] - mx);
    return mx + std::log(s);
}
void log_softmax_rows(float* x, int n, int d) {
    #pragma omp parallel for if (n > 16) num_threads(num_threads())
    for (int r = 0; r < n; r++) { float* p = x + (size_t)r * d; const float l = logsumexp_row(p, d); for (int c = 0; c < d; c++) p[c] -= l; }
}

}  // namespace cpu
}  // namespace aq
