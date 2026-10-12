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

// ---------------------------------------------------------------- vector width
// The decoder is built twice, aquadecoder_avx2.dll (/arch:AVX2: AVX2, FMA, F16C) and aquadecoder_sse.dll (SSE4.1, for
// CPUs without AVX2); aquadecoder.dll picks one at run time (aq_dispatch.cpp).  The kernels are written once on vf:
// 8 lanes of AVX2 or 4 of SSE.  The SSE build has no FMA, so a * b + c rounds twice there.
#if defined(__AVX2__)
typedef __m256 vf; typedef __m256i vi; static constexpr int VW = 8;
static inline vf vload(const float* p) { return _mm256_loadu_ps(p); }
static inline void vstore(float* p, vf v) { _mm256_storeu_ps(p, v); }
static inline vf vzero() { return _mm256_setzero_ps(); }
static inline vf vset(float x) { return _mm256_set1_ps(x); }
static inline vf vadd(vf a, vf b) { return _mm256_add_ps(a, b); }
static inline vf vsub(vf a, vf b) { return _mm256_sub_ps(a, b); }
static inline vf vmul(vf a, vf b) { return _mm256_mul_ps(a, b); }
static inline vf vmin(vf a, vf b) { return _mm256_min_ps(a, b); }
static inline vf vmax(vf a, vf b) { return _mm256_max_ps(a, b); }
static inline vf vand(vf a, vf b) { return _mm256_and_ps(a, b); }
static inline vf vor(vf a, vf b) { return _mm256_or_ps(a, b); }
static inline vf vandnot(vf a, vf b) { return _mm256_andnot_ps(a, b); }
static inline vf vfma(vf a, vf b, vf c) { return _mm256_fmadd_ps(a, b, c); }      // a * b + c
static inline vf vfnma(vf a, vf b, vf c) { return _mm256_fnmadd_ps(a, b, c); }    // c - a * b
static inline vf vfloor(vf a) { return _mm256_floor_ps(a); }
static inline vf vgt(vf a, vf b) { return _mm256_cmp_ps(a, b, _CMP_GT_OQ); }
static inline vf vblend(vf a, vf b, vf mask) { return _mm256_blendv_ps(a, b, mask); }
static inline vf vpow2i(vf n) { return _mm256_castsi256_ps(_mm256_slli_epi32(_mm256_add_epi32(_mm256_cvttps_epi32(n), _mm256_set1_epi32(127)), 23)); }
static inline void vstore_round(int32_t* p, vf v) { _mm256_storeu_si256((__m256i*)p, _mm256_cvtps_epi32(v)); }   // nearest even
static inline vf vload_i8(const int8_t* p) { return _mm256_cvtepi32_ps(_mm256_cvtepi8_epi32(_mm_loadl_epi64((const __m128i*)p))); }
static inline float hsum(vf v) { __m128 a = _mm_add_ps(_mm256_castps256_ps128(v), _mm256_extractf128_ps(v, 1)); a = _mm_add_ps(a, _mm_movehl_ps(a, a)); a = _mm_add_ss(a, _mm_shuffle_ps(a, a, 1)); return _mm_cvtss_f32(a); }
#else
typedef __m128 vf; typedef __m128i vi; static constexpr int VW = 4;
static inline vf vload(const float* p) { return _mm_loadu_ps(p); }
static inline void vstore(float* p, vf v) { _mm_storeu_ps(p, v); }
static inline vf vzero() { return _mm_setzero_ps(); }
static inline vf vset(float x) { return _mm_set1_ps(x); }
static inline vf vadd(vf a, vf b) { return _mm_add_ps(a, b); }
static inline vf vsub(vf a, vf b) { return _mm_sub_ps(a, b); }
static inline vf vmul(vf a, vf b) { return _mm_mul_ps(a, b); }
static inline vf vmin(vf a, vf b) { return _mm_min_ps(a, b); }
static inline vf vmax(vf a, vf b) { return _mm_max_ps(a, b); }
static inline vf vand(vf a, vf b) { return _mm_and_ps(a, b); }
static inline vf vor(vf a, vf b) { return _mm_or_ps(a, b); }
static inline vf vandnot(vf a, vf b) { return _mm_andnot_ps(a, b); }
static inline vf vfma(vf a, vf b, vf c) { return _mm_add_ps(_mm_mul_ps(a, b), c); }
static inline vf vfnma(vf a, vf b, vf c) { return _mm_sub_ps(c, _mm_mul_ps(a, b)); }
static inline vf vfloor(vf a) { return _mm_floor_ps(a); }
static inline vf vgt(vf a, vf b) { return _mm_cmpgt_ps(a, b); }
static inline vf vblend(vf a, vf b, vf mask) { return _mm_blendv_ps(a, b, mask); }
static inline vf vpow2i(vf n) { return _mm_castsi128_ps(_mm_slli_epi32(_mm_add_epi32(_mm_cvttps_epi32(n), _mm_set1_epi32(127)), 23)); }
static inline void vstore_round(int32_t* p, vf v) { _mm_storeu_si128((__m128i*)p, _mm_cvtps_epi32(v)); }
static inline vf vload_i8(const int8_t* p) { int32_t w; std::memcpy(&w, p, 4); return _mm_cvtepi32_ps(_mm_cvtepi8_epi32(_mm_cvtsi32_si128(w))); }
static inline float hsum(vf a) { a = _mm_add_ps(a, _mm_movehl_ps(a, a)); a = _mm_add_ss(a, _mm_shuffle_ps(a, a, 1)); return _mm_cvtss_f32(a); }
#endif

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
#if defined(__AVX2__)
float h2f(uint16_t h) { return _mm_cvtss_f32(_mm_cvtph_ps(_mm_cvtsi32_si128(h))); }
uint16_t f2h(float f) { return (uint16_t)_mm_cvtsi128_si32(_mm_cvtps_ph(_mm_set_ss(f), _MM_FROUND_TO_NEAREST_INT)); }
void h2f_n(const uint16_t* s, float* d, size_t n) { size_t i = 0; for (; i + 8 <= n; i += 8) _mm256_storeu_ps(d + i, _mm256_cvtph_ps(_mm_loadu_si128((const __m128i*)(s + i)))); for (; i < n; i++) d[i] = h2f(s[i]); }
void f2h_n(const float* s, uint16_t* d, size_t n) { size_t i = 0; for (; i + 8 <= n; i += 8) _mm_storeu_si128((__m128i*)(d + i), _mm256_cvtps_ph(_mm256_loadu_ps(s + i), _MM_FROUND_TO_NEAREST_INT)); for (; i < n; i++) d[i] = f2h(s[i]); }
#else
// no F16C: the bit conversions, round to nearest even as vcvtps2ph (f2h after F. Giesen's float_to_half_fast3_rtne)
float h2f(uint16_t h) {
    uint32_t s = (uint32_t)(h & 0x8000) << 16, e = (h >> 10) & 0x1f, m = h & 0x3ff, f;
    if (e == 0) { if (m == 0) f = s; else { e = 1; while (!(m & 0x400)) { m <<= 1; e--; } m &= 0x3ff; f = s | ((e + 112) << 23) | (m << 13); } }
    else if (e == 31) f = s | 0x7f800000u | (m << 13);
    else f = s | ((e + 112) << 23) | (m << 13);
    float r; std::memcpy(&r, &f, 4); return r;
}
uint16_t f2h(float x) {
    uint32_t f; std::memcpy(&f, &x, 4); const uint32_t sign = f & 0x80000000u; f ^= sign; uint32_t o;
    if (f >= (127u + 16) << 23) o = f > (255u << 23) ? 0x7e00 : 0x7c00;                               // overflow -> inf, NaN stays NaN
    else if (f < (113u << 23)) { const uint32_t magic = ((127u - 15) + (23 - 10) + 1) << 23; float a, b; std::memcpy(&a, &f, 4); std::memcpy(&b, &magic, 4);
        a += b; uint32_t u; std::memcpy(&u, &a, 4); o = u - magic; }                                     // subnormal or zero
    else { const uint32_t odd = (f >> 13) & 1; f += ((uint32_t)(15 - 127) << 23) + 0xfff; f += odd; o = f >> 13; }
    return (uint16_t)(o | (sign >> 16));
}
void h2f_n(const uint16_t* s, float* d, size_t n) { for (size_t i = 0; i < n; i++) d[i] = h2f(s[i]); }
void f2h_n(const float* s, uint16_t* d, size_t n) { for (size_t i = 0; i < n; i++) d[i] = f2h(s[i]); }
#endif
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
#if !defined(__AVX2__)
    if (bits == 4 && group % 8 == 0) {
        for (int w = 0; w < words; w++) {
            const int g = (w * 8) / group; const uint32_t v = row[w]; const __m128 s4 = _mm_set1_ps(sc[g]), b4 = _mm_set1_ps(bi[g]);
            const __m128i lo = _mm_setr_epi32(v & 15, (v >> 4) & 15, (v >> 8) & 15, (v >> 12) & 15), hi = _mm_setr_epi32((v >> 16) & 15, (v >> 20) & 15, (v >> 24) & 15, v >> 28);
            _mm_storeu_ps(dst + w * 8, _mm_add_ps(_mm_mul_ps(_mm_cvtepi32_ps(lo), s4), b4)); _mm_storeu_ps(dst + w * 8 + 4, _mm_add_ps(_mm_mul_ps(_mm_cvtepi32_ps(hi), s4), b4));
        }
        return;
    }
#else
    if (bits == 4 && group % 8 == 0) {
        const __m256i shifts = _mm256_setr_epi32(0, 4, 8, 12, 16, 20, 24, 28), m4 = _mm256_set1_epi32(0xF);
        for (int w = 0; w < words; w++) {
            const int g = (w * 8) / group; __m256i v = _mm256_and_si256(_mm256_srlv_epi32(_mm256_set1_epi32((int)row[w]), shifts), m4);
            _mm256_storeu_ps(dst + w * 8, _mm256_fmadd_ps(_mm256_cvtepi32_ps(v), _mm256_set1_ps(sc[g]), _mm256_set1_ps(bi[g])));
        }
        return;
    }
#endif
    for (int i = 0; i < in; i++) { uint32_t v = (row[i / per] >> ((i % per) * bits)) & mask; dst[i] = (float)v * sc[i / group] + bi[i / group]; }
}

// ---------------------------------------------------------------- GEMM
// R rows of x against C rows of w, the whole K: R*C dot products, one register each
template <int R, int C>
static inline void micro(const float* x, int K, const float* w, float* y, int ldy, const float* bias, bool acc) {
    vf s[R][C];
    for (int r = 0; r < R; r++) for (int c = 0; c < C; c++) s[r][c] = vzero();
    int k = 0;
    for (; k + VW <= K; k += VW) {
        vf wv[C]; for (int c = 0; c < C; c++) wv[c] = vload(w + (size_t)c * K + k);
        for (int r = 0; r < R; r++) { vf xv = vload(x + (size_t)r * K + k); for (int c = 0; c < C; c++) s[r][c] = vfma(xv, wv[c], s[r][c]); }
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
#if defined(__AVX2__)
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
#else
// SSE: 16 registers hold two rows x the panel's 16 columns (8 sums, 4 weights, 1 broadcast), so 6 rows are 3 passes
static inline void mk6(const float* x, int K, const float* panel, float* y, int ldy, int nr, int nc, const float* bias, bool acc) {
    alignas(16) float t[MR][NR];
    for (int r0 = 0; r0 < MR; r0 += 2) {
        __m128 a0 = _mm_setzero_ps(), a1 = a0, a2 = a0, a3 = a0, b0 = a0, b1 = a0, b2 = a0, b3 = a0;
        const float *xa = x + (size_t)r0 * K, *xb = xa + K;
        for (int k = 0; k < K; k++) {
            const float* pw = panel + (size_t)k * NR;
            const __m128 w0 = _mm_loadu_ps(pw), w1 = _mm_loadu_ps(pw + 4), w2 = _mm_loadu_ps(pw + 8), w3 = _mm_loadu_ps(pw + 12);
            __m128 v = _mm_set1_ps(xa[k]); a0 = _mm_add_ps(a0, _mm_mul_ps(v, w0)); a1 = _mm_add_ps(a1, _mm_mul_ps(v, w1)); a2 = _mm_add_ps(a2, _mm_mul_ps(v, w2)); a3 = _mm_add_ps(a3, _mm_mul_ps(v, w3));
            v = _mm_set1_ps(xb[k]); b0 = _mm_add_ps(b0, _mm_mul_ps(v, w0)); b1 = _mm_add_ps(b1, _mm_mul_ps(v, w1)); b2 = _mm_add_ps(b2, _mm_mul_ps(v, w2)); b3 = _mm_add_ps(b3, _mm_mul_ps(v, w3));
        }
        _mm_store_ps(t[r0], a0); _mm_store_ps(t[r0] + 4, a1); _mm_store_ps(t[r0] + 8, a2); _mm_store_ps(t[r0] + 12, a3);
        _mm_store_ps(t[r0 + 1], b0); _mm_store_ps(t[r0 + 1] + 4, b1); _mm_store_ps(t[r0 + 1] + 8, b2); _mm_store_ps(t[r0 + 1] + 12, b3);
        if (r0 + 2 >= nr) break;                                                                       // the rest is padding
    }
    for (int r = 0; r < nr; r++) { float* o = y + (size_t)r * ldy;
        for (int j = 0; j < nc; j++) { const float v = t[r][j] + (bias ? bias[j] : 0.0f); o[j] = acc ? o[j] + v : v; } }
}
#endif
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
    #pragma omp parallel for schedule(dynamic) num_threads(num_threads())
    for (int t = 0; t < tiles; t++) tile(t);
}

// ---- 4-bit weight x 8-bit activation, nothing dequantised.  w = q * s + b over a group of 64, so for one group
//   x . w = s * (x . q) + b * sum(x)  and, with x as int8 (x = xq * dx per group),  x . q ~ dx * (xq . q)  on integers.
// A packed byte holds k = 2m (low nibble) and k = 2m + 1 (high), so each group of x is stored as its 32 even entries
// then its 32 odd ones: the nibbles go into maddubs without a shuffle.  AQ_Q8=0 falls back to dequantising (A/B).
static constexpr int QG = 64;
struct XQ { int K = 0, ng = 0; std::vector<int8_t> q; std::vector<float> d, sum; };
// in_order: the plain layout gemm_i8 reads; otherwise each group's even entries, then its odd ones (gemm_q4q8)
static void quant_x(const float* x, int n, int K, XQ& o, bool in_order = false) {
    o.K = K; o.ng = K / QG; o.q.resize((size_t)n * K); o.d.resize((size_t)n * o.ng); o.sum.resize((size_t)n * o.ng);
    #pragma omp parallel for num_threads(num_threads()) if ((long long)n * K >= 65536)
    for (int r = 0; r < n; r++) for (int g = 0; g < o.ng; g++) {
        const float* xs = x + (size_t)r * K + (size_t)g * QG; int8_t* dst = o.q.data() + (size_t)r * K + (size_t)g * QG;
        vf mx8 = vzero(), s8 = vzero(); const vf sign = vset(-0.0f);
        for (int k = 0; k < QG; k += VW) { const vf v = vload(xs + k); mx8 = vmax(mx8, vandnot(sign, v)); s8 = vadd(s8, v); }
        float t[VW]; vstore(t, mx8); float mx = 0; for (float v : t) mx = std::max(mx, v); const float s = hsum(s8);
        const float d = mx / 127.0f, inv = d > 0 ? 1.0f / d : 0.0f;
        if (in_order) { int32_t q[QG]; const vf i8 = vset(inv); for (int k = 0; k < QG; k += VW) vstore_round(q + k, vmul(vload(xs + k), i8)); for (int k = 0; k < QG; k++) dst[k] = (int8_t)q[k]; }
        else for (int m = 0; m < QG / 2; m++) { dst[m] = (int8_t)std::lrintf(xs[2 * m] * inv); dst[QG / 2 + m] = (int8_t)std::lrintf(xs[2 * m + 1] * inv); }
        o.d[(size_t)r * o.ng + g] = d; o.sum[(size_t)r * o.ng + g] = s;
    }
}
#if defined(__AVX2__)
// one group: the 32 packed bytes against the even and odd halves of x, 8 int32 partial sums
static inline __m256i q4_dot(__m256i lo, __m256i hi, const int8_t* xg) {
    const __m256i ones = _mm256_set1_epi16(1);
    return _mm256_add_epi32(_mm256_madd_epi16(_mm256_maddubs_epi16(lo, _mm256_loadu_si256((const __m256i*)xg)), ones),
                            _mm256_madd_epi16(_mm256_maddubs_epi16(hi, _mm256_loadu_si256((const __m256i*)(xg + 32))), ones));
}
// weight row w (K / 2 bytes) against x rows [r0, r0 + R): the integer part of every group, scaled into one fp32 sum each
template <int R> static inline void q4_row(const uint8_t* w, const float* s, const XQ& X, int r0, float* out) {
    const __m256i m4 = _mm256_set1_epi8(0x0F); __m256 acc[R]; for (int r = 0; r < R; r++) acc[r] = _mm256_setzero_ps();
    for (int g = 0; g < X.ng; g++) {
        const __m256i p = _mm256_loadu_si256((const __m256i*)(w + (size_t)g * 32)), lo = _mm256_and_si256(p, m4), hi = _mm256_and_si256(_mm256_srli_epi16(p, 4), m4);
        for (int r = 0; r < R; r++) { const size_t xr = (size_t)(r0 + r);
            acc[r] = _mm256_fmadd_ps(_mm256_cvtepi32_ps(q4_dot(lo, hi, X.q.data() + xr * X.K + (size_t)g * QG)), _mm256_set1_ps(s[g] * X.d[xr * X.ng + g]), acc[r]); }
    }
    for (int r = 0; r < R; r++) out[r] = hsum(acc[r]);
}
#else
static inline __m128i q4_dot(__m128i lo, __m128i hi, const int8_t* xe, const int8_t* xo) {
    const __m128i ones = _mm_set1_epi16(1);
    return _mm_add_epi32(_mm_madd_epi16(_mm_maddubs_epi16(lo, _mm_loadu_si128((const __m128i*)xe)), ones),
                         _mm_madd_epi16(_mm_maddubs_epi16(hi, _mm_loadu_si128((const __m128i*)xo)), ones));
}
template <int R> static inline void q4_row(const uint8_t* w, const float* s, const XQ& X, int r0, float* out) {
    const __m128i m4 = _mm_set1_epi8(0x0F); __m128 acc[R]; for (int r = 0; r < R; r++) acc[r] = _mm_setzero_ps();
    for (int g = 0; g < X.ng; g++) {
        const __m128i p0 = _mm_loadu_si128((const __m128i*)(w + (size_t)g * 32)), p1 = _mm_loadu_si128((const __m128i*)(w + (size_t)g * 32 + 16));
        const __m128i lo0 = _mm_and_si128(p0, m4), hi0 = _mm_and_si128(_mm_srli_epi16(p0, 4), m4), lo1 = _mm_and_si128(p1, m4), hi1 = _mm_and_si128(_mm_srli_epi16(p1, 4), m4);
        for (int r = 0; r < R; r++) { const size_t xr = (size_t)(r0 + r); const int8_t* xg = X.q.data() + xr * X.K + (size_t)g * QG;
            const __m128i t = _mm_add_epi32(q4_dot(lo0, hi0, xg, xg + 32), q4_dot(lo1, hi1, xg + 16, xg + 48));
            acc[r] = _mm_add_ps(acc[r], _mm_mul_ps(_mm_cvtepi32_ps(t), _mm_set1_ps(s[g] * X.d[xr * X.ng + g]))); }
    }
    for (int r = 0; r < R; r++) out[r] = hsum(acc[r]);
}
#endif
static bool q8_on() { static const bool on = [] { const char* e = std::getenv("AQ_Q8"); return !(e && std::string(e) == "0"); }(); return on; }
static void gemm_q4q8(const float* x, int n, const QWeight& W, float* y, const float* bias, const std::vector<int32_t>* rows) {
    const int m = rows ? (int)rows->size() : W.out, K = W.in, ng = K / QG;
    // dynamic: a worker the OS has preempted holds up a static share; with chunks of 8 rows the others take it over
    thread_local XQ tls; quant_x(x, n, K, tls); const XQ* xp = &tls;   // the caller's copy: inside the loop tls would name each worker's own
    #pragma omp parallel for schedule(dynamic, 8) num_threads(num_threads()) if ((long long)m * n * K > 400000)
    for (int j = 0; j < m; j++) {
        const XQ& X = *xp; const int wr = rows ? (*rows)[j] : j; const uint8_t* w = (const uint8_t*)(W.q + (size_t)wr * (K / 8));
        const float* s = W.scales.data() + (size_t)wr * ng; const float* b = W.biases.data() + (size_t)wr * ng; const float bj = bias ? bias[wr] : 0.0f;
        float t[4]; int r = 0;
        for (; r + 4 <= n; r += 4) { q4_row<4>(w, s, X, r, t); for (int i = 0; i < 4; i++) y[(size_t)(r + i) * m + j] = t[i]; }
        for (; r < n; r++) { q4_row<1>(w, s, X, r, t); y[(size_t)r * m + j] = t[0]; }
        for (r = 0; r < n; r++) { const float* xs = X.sum.data() + (size_t)r * ng; float a = bj; for (int g = 0; g < ng; g++) a += b[g] * xs[g]; y[(size_t)r * m + j] += a; }
    }
}

// ---- int8 weight x int8 activation, both symmetric per group of 64.  maddubs takes one unsigned operand: |w| against
// x carrying w's sign (sign_epi8), so the pair sums stay within int16 (2 * 127 * 127).
I8Weight quant_weight_i8(const float* w, int m, int K) {
    I8Weight W; W.out = m; W.in = K; const int ng = K / QG; W.q.resize((size_t)m * K); W.scales.resize((size_t)m * ng);
    for (int j = 0; j < m; j++) for (int g = 0; g < ng; g++) {
        const float* src = w + (size_t)j * K + (size_t)g * QG; float mx = 0; for (int k = 0; k < QG; k++) mx = std::max(mx, std::fabs(src[k]));
        const float d = mx / 127.0f, inv = d > 0 ? 1.0f / d : 0.0f; W.scales[(size_t)j * ng + g] = d;
        for (int k = 0; k < QG; k++) W.q[(size_t)j * K + (size_t)g * QG + k] = (int8_t)std::lrintf(src[k] * inv);
    }
    return W;
}
#if defined(__AVX2__)
static inline __m256i i8_dot(__m256i w, const int8_t* x) {      // 32 bytes -> 8 int32 partial sums
    const __m256i xv = _mm256_loadu_si256((const __m256i*)x);
    return _mm256_madd_epi16(_mm256_maddubs_epi16(_mm256_sign_epi8(w, w), _mm256_sign_epi8(xv, w)), _mm256_set1_epi16(1));
}
template <int R> static inline void i8_row(const int8_t* w, const float* s, const XQ& X, int r0, float* out) {
    __m256 acc[R]; for (int r = 0; r < R; r++) acc[r] = _mm256_setzero_ps();
    for (int g = 0; g < X.ng; g++) {
        const __m256i w0 = _mm256_loadu_si256((const __m256i*)(w + (size_t)g * QG)), w1 = _mm256_loadu_si256((const __m256i*)(w + (size_t)g * QG + 32));
        for (int r = 0; r < R; r++) { const size_t xr = (size_t)(r0 + r); const int8_t* xg = X.q.data() + xr * X.K + (size_t)g * QG;
            acc[r] = _mm256_fmadd_ps(_mm256_cvtepi32_ps(_mm256_add_epi32(i8_dot(w0, xg), i8_dot(w1, xg + 32))), _mm256_set1_ps(s[g] * X.d[xr * X.ng + g]), acc[r]); }
    }
    for (int r = 0; r < R; r++) out[r] = hsum(acc[r]);
}
#else
static inline __m128i i8_dot(__m128i w, const int8_t* x) {      // 16 bytes -> 4 int32 partial sums
    const __m128i xv = _mm_loadu_si128((const __m128i*)x);
    return _mm_madd_epi16(_mm_maddubs_epi16(_mm_sign_epi8(w, w), _mm_sign_epi8(xv, w)), _mm_set1_epi16(1));
}
template <int R> static inline void i8_row(const int8_t* w, const float* s, const XQ& X, int r0, float* out) {
    __m128 acc[R]; for (int r = 0; r < R; r++) acc[r] = _mm_setzero_ps();
    for (int g = 0; g < X.ng; g++) { const int8_t* wg = w + (size_t)g * QG;
        const __m128i w0 = _mm_loadu_si128((const __m128i*)wg), w1 = _mm_loadu_si128((const __m128i*)(wg + 16)), w2 = _mm_loadu_si128((const __m128i*)(wg + 32)), w3 = _mm_loadu_si128((const __m128i*)(wg + 48));
        for (int r = 0; r < R; r++) { const size_t xr = (size_t)(r0 + r); const int8_t* xg = X.q.data() + xr * X.K + (size_t)g * QG;
            const __m128i t = _mm_add_epi32(_mm_add_epi32(i8_dot(w0, xg), i8_dot(w1, xg + 16)), _mm_add_epi32(i8_dot(w2, xg + 32), i8_dot(w3, xg + 48)));
            acc[r] = _mm_add_ps(acc[r], _mm_mul_ps(_mm_cvtepi32_ps(t), _mm_set1_ps(s[g] * X.d[xr * X.ng + g]))); }
    }
    for (int r = 0; r < R; r++) out[r] = hsum(acc[r]);
}
#endif
void gemm_i8(const float* x, int n, const I8Weight& W, float* y, const float* bias, bool acc) {
    const int m = W.out, K = W.in, ng = K / QG; if (n <= 0 || m <= 0) return;
    thread_local XQ tls; quant_x(x, n, K, tls, true); const XQ* xp = &tls;
    #pragma omp parallel for schedule(dynamic, 8) num_threads(num_threads()) if ((long long)m * n * K > 400000)
    for (int j = 0; j < m; j++) {
        const XQ& X = *xp; const int8_t* w = W.q.data() + (size_t)j * K; const float* s = W.scales.data() + (size_t)j * ng; const float bj = bias ? bias[j] : 0.0f;
        float t[4]; int r = 0;
        auto put = [&](int row, float v) { float& o = y[(size_t)row * m + j]; o = acc ? o + v + bj : v + bj; };
        for (; r + 4 <= n; r += 4) { i8_row<4>(w, s, X, r, t); for (int i = 0; i < 4; i++) put(r + i, t[i]); }
        for (; r < n; r++) { i8_row<1>(w, s, X, r, t); put(r, t[0]); }
    }
}

// the quantised weight: each thread dequantises a panel of NR rows straight into the packed layout, then runs every x row
void gemm_q(const float* x, int n, const QWeight& W, float* y, const float* bias, const std::vector<int32_t>* rows) {
    const int m = rows ? (int)rows->size() : W.out, K = W.in; if (n <= 0 || m <= 0) return;
    if (W.bits == 4 && W.group == QG && K % QG == 0 && q8_on()) { gemm_q4q8(x, n, W, y, bias, rows); return; }
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
    int c = 0; vf s8 = vzero();
    for (; c + VW <= d; c += VW) s8 = vadd(s8, vload(xr + c));
    float s = hsum(s8); for (; c < d; c++) s += xr[c]; const float mean = s / d;
    const vf m8 = vset(mean); vf q8 = vzero(); c = 0;
    for (; c + VW <= d; c += VW) { const vf t = vsub(vload(xr + c), m8); q8 = vfma(t, t, q8); }
    float q = hsum(q8); for (; c < d; c++) { const float t = xr[c] - mean; q += t * t; }
    const float rstd = 1.0f / std::sqrt(q / d + eps); const vf r8 = vset(rstd); c = 0;
    for (; c + VW <= d; c += VW) vstore(yr + c, vfma(vmul(vsub(vload(xr + c), m8), r8), vload(g + c), vload(b + c)));
    for (; c < d; c++) yr[c] = (xr[c] - mean) * rstd * g[c] + b[c];
}
void layer_norm(const float* x, int n, int d, const float* g, const float* b, float eps, float* y) {
    #pragma omp parallel for num_threads(num_threads()) if ((long long)n * d > 65536)
    for (int r = 0; r < n; r++) layer_norm_row(x + (size_t)r * d, d, g, b, eps, y + (size_t)r * d);
}
float quant_i8(const float* src, int n, int8_t* dst) {
    int c = 0; vf mx8 = vzero(); const vf sign = vset(-0.0f);
    for (; c + VW <= n; c += VW) mx8 = vmax(mx8, vandnot(sign, vload(src + c)));
    float t[VW]; vstore(t, mx8); float mx = 0; for (float v : t) mx = std::max(mx, v); for (; c < n; c++) mx = std::max(mx, std::fabs(src[c]));
    const float s = std::max(mx / 127.0f, 1e-6f), inv = 1.0f / s; const vf i8 = vset(inv); c = 0;
    int32_t q[VW];
    for (; c + VW <= n; c += VW) { vstore_round(q, vmul(vload(src + c), i8)); for (int j = 0; j < VW; j++) dst[c + j] = (int8_t)q[j]; }   // round to nearest even, as rint
    for (; c < n; c++) dst[c] = (int8_t)std::nearbyint(src[c] * inv);
    return s;
}
float dot_f32_i8(const float* q, const int8_t* k, int n) {
    int c = 0; vf s = vzero(); for (; c + VW <= n; c += VW) s = vfma(vload(q + c), vload_i8(k + c), s);
    float r = hsum(s); for (; c < n; c++) r += q[c] * (float)k[c]; return r;
}
void scale_add_i8(float* acc, float cf, float w, const int8_t* v, int n) {
    int c = 0; const vf c8 = vset(cf), w8 = vset(w);
    for (; c + VW <= n; c += VW) vstore(acc + c, vfma(w8, vload_i8(v + c), vmul(vload(acc + c), c8)));
    for (; c < n; c++) acc[c] = acc[c] * cf + w * (float)v[c];
}
// exp on every lane (Cephes polynomial; |rel err| ~1e-7 over the float range)
static inline vf vexp(vf x) {
    x = vmin(x, vset(88.3762626647949f)); x = vmax(x, vset(-88.3762626647949f));
    vf fx = vfloor(vfma(x, vset(1.44269504088896341f), vset(0.5f)));
    x = vfnma(fx, vset(0.693359375f), x); x = vfnma(fx, vset(-2.12194440e-4f), x);
    vf y = vset(1.9875691500E-4f);
    y = vfma(y, x, vset(1.3981999507E-3f)); y = vfma(y, x, vset(8.3334519073E-3f));
    y = vfma(y, x, vset(4.1665795894E-2f)); y = vfma(y, x, vset(1.6666665459E-1f));
    y = vfma(y, x, vset(5.0000001201E-1f)); y = vfma(y, vmul(x, x), vadd(x, vset(1.0f)));
    return vmul(y, vpow2i(fx));
}
// MLX's erf (mlx/backend/metal/kernels/erf.h), both branches on every lane and a blend
static inline vf verf(vf a) {
    const vf t = vandnot(vset(-0.0f), a), s = vmul(a, a);
    vf r = vfma(vset(-1.72853470e-5f), t, vset(3.83197126e-4f));
    vf u = vfma(vset(-3.88396438e-3f), t, vset(2.42546219e-2f));
    r = vfma(r, s, u); r = vfma(r, t, vset(-1.06777877e-1f)); r = vfma(r, t, vset(-6.34846687e-1f));
    r = vfma(r, t, vset(-1.28717512e-1f)); r = vfma(r, t, vsub(vzero(), t));
    r = vsub(vset(1.0f), vexp(r));                                                                  // -(exp(r) - 1)
    r = vor(r, vand(a, vset(-0.0f)));                                                               // copysign (r >= 0 here)
    vf p = vset(-5.96761703e-4f);
    p = vfma(p, s, vset(4.99119423e-3f)); p = vfma(p, s, vset(-2.67681349e-2f));
    p = vfma(p, s, vset(1.12819925e-1f)); p = vfma(p, s, vset(-3.76125336e-1f));
    p = vfma(p, s, vset(1.28379166e-1f)); p = vfma(p, a, a);
    return vblend(p, r, vgt(t, vset(0.927734375f)));
}
void gelu_inplace(float* x, size_t n) {
    auto body = [](float* p, size_t k) { size_t i = 0;
        for (; i + VW <= k; i += VW) { vf v = vload(p + i); vstore(p + i, vmul(vmul(vset(0.5f), v), vadd(vset(1.0f), verf(vmul(v, vset(0.70710678118654752f)))))); }
        for (; i < k; i++) p[i] = 0.5f * p[i] * (1.0f + std::erf(p[i] * 0.70710678118654752f)); };
    if (n < 65536) { body(x, n); return; }
    const int chunks = num_threads() * 4; const size_t per = ((n / chunks) + 7) & ~(size_t)7;   // a multiple of either width
    #pragma omp parallel for num_threads(num_threads())
    for (int c = 0; c < chunks; c++) { const size_t a = (size_t)c * per; if (a < n) body(x + a, std::min(per, n - a)); }
}
float logsumexp_row(const float* x, int n) {
    float mx = -INFINITY; for (int i = 0; i < n; i++) mx = std::max(mx, x[i]);
    if (!std::isfinite(mx)) return mx;
    vf acc = vzero(), m = vset(mx); int i = 0;
    for (; i + VW <= n; i += VW) acc = vadd(acc, vexp(vsub(vload(x + i), m)));
    float s = hsum(acc); for (; i < n; i++) s += std::exp(x[i] - mx);
    return mx + std::log(s);
}
void log_softmax_rows(float* x, int n, int d) {
    #pragma omp parallel for if (n > 16) num_threads(num_threads())
    for (int r = 0; r < n; r++) { float* p = x + (size_t)r * d; const float l = logsumexp_row(p, d); for (int c = 0; c < d; c++) p[c] -= l; }
}

}  // namespace cpu
}  // namespace aq
