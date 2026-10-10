// The two v2 models on MLX (see v2_model.h).  Numerics follow decoder/v2/mlx_engine.py: weights and matmuls fp16
// (syllable linears quantised, 4-bit in the shipped model), residual stream fp16 with LayerNorm in fp32 inside the fused kernel, span head and
// log-softmax in fp32.  The character-model step: bias adds folded into addmm, GELU one kernel, per-layer K/V tables.
#include "v2_model.h"
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <chrono>
#include <map>
#include <optional>

namespace aq {
using namespace mlx::core;
static const float NEG = -1e4f;

// GELU as one Metal kernel, fp32 math on fp16 in/out: the generic chain is six elementwise fp32 kernels and a large
// transient.  Formula: 0.5 x (1 + erf(x / sqrt 2)).
static const std::string ERF_HDR = R"(
// MLX's Metal erf (mlx/backend/metal/kernels/erf.h, faithfully rounded, <1 ulp); expm1f replaced by exp - 1 (its argument is <= -0.93 there)
float aq_erf(float a) {
  float r, s, t, u; t = metal::abs(a); s = a * a;
  if (t > 0.927734375f) {
    r = metal::fma(-1.72853470e-5f, t, 3.83197126e-4f); u = metal::fma(-3.88396438e-3f, t, 2.42546219e-2f); r = metal::fma(r, s, u);
    r = metal::fma(r, t, -1.06777877e-1f); r = metal::fma(r, t, -6.34846687e-1f); r = metal::fma(r, t, -1.28717512e-1f); r = metal::fma(r, t, -t);
    r = -(metal::exp(r) - 1.0f); r = metal::copysign(r, a);
  } else {
    r = -5.96761703e-4f; r = metal::fma(r, s, 4.99119423e-3f); r = metal::fma(r, s, -2.67681349e-2f); r = metal::fma(r, s, 1.12819925e-1f);
    r = metal::fma(r, s, -3.76125336e-1f); r = metal::fma(r, s, 1.28379166e-1f); r = metal::fma(r, a, a);
  }
  return r;
}
)";
static const std::string GELU_SRC = R"(
    uint i = thread_position_in_grid.x; if (i >= (uint)n[0]) return;
    float v = (float)x[i]; y[i] = (half)(0.5f * v * (1.0f + aq_erf(v * 0.70710678118654752f)));
)";
static const std::string GELU32_SRC = R"(
    uint i = thread_position_in_grid.x; if (i >= (uint)n[0]) return;
    float v = x[i]; y[i] = 0.5f * v * (1.0f + aq_erf(v * 0.70710678118654752f));
)";
array aq_gelu_fused(const array& x) {
    static fast::CustomKernelFunction k16 = fast::metal_kernel("aq_gelu16", {"x", "n"}, {"y"}, GELU_SRC, ERF_HDR), k32 = fast::metal_kernel("aq_gelu32", {"x", "n"}, {"y"}, GELU32_SRC, ERF_HDR);
    if (x.dtype() == float16 || x.dtype() == float32) { int total = (int)x.size(); array xc = contiguous(x); auto& k = x.dtype() == float16 ? k16 : k32;
        return reshape(k({reshape(xc, {total}), array({total}, {1}, int32)}, {Shape{total}}, {x.dtype()}, std::make_tuple(((total + 255) / 256) * 256, 1, 1), std::make_tuple(256, 1, 1), {}, std::nullopt, false, {})[0], x.shape()); }
    array xf = astype(x, float32); return astype(multiply(multiply(xf, array(0.5f)), add(array(1.0f), erf(divide(xf, array(std::sqrt(2.0f)))))), x.dtype()); }

static std::string read_file(const std::string& p) { std::ifstream f(p); if (!f) throw std::runtime_error("cannot read " + p); std::stringstream ss; ss << f.rdbuf(); return ss.str(); }
static double jnum(const std::string& js, const std::string& sec, const std::string& key, double dflt) {   // "sec": { ... "key": number ... }
    size_t sp = sec.empty() ? 0 : js.find("\"" + sec + "\""); if (sp == std::string::npos) return dflt;
    size_t pos = js.find("\"" + key + "\"", sp); if (pos == std::string::npos) return dflt; pos = js.find(':', pos); if (pos == std::string::npos) return dflt;
    size_t i = pos + 1; while (i < js.size() && js[i] == ' ') i++; if (js.compare(i, 4, "null") == 0) return dflt; return std::atof(js.c_str() + i);
}

V2Model::V2Model(const std::string& dir) {
    auto loaded = load_safetensors(dir + "/model.safetensors"); w_ = std::move(loaded.first);
    has_char_head_ = w_.count("syl.char_head.weight.q") || w_.count("syl.char_head.weight");
    std::string js = read_file(dir + "/model.json");
    cfg_.syl_d = (int)jnum(js, "syl", "d", 1024); cfg_.syl_layers = (int)jnum(js, "syl", "layers", 12); cfg_.syl_heads = (int)jnum(js, "syl", "heads", 16); cfg_.n_syl = (int)jnum(js, "syl", "n_syl", 1784); cfg_.n_keys = (int)jnum(js, "syl", "n_keys", 44); cfg_.syl_eps = (float)jnum(js, "syl", "ln_eps", 1e-12);
    n_char_ = (int)jnum(js, "syl", "n_char", 0);
    if (js.find("\"quant\": {") != std::string::npos) { cfg_.quant_bits = (int)jnum(js, "quant", "bits", 6); cfg_.quant_group = (int)jnum(js, "quant", "group_size", 64); }
    cfg_.chr_d = (int)jnum(js, "chr", "d", 384); cfg_.chr_layers = (int)jnum(js, "chr", "layers", 6); cfg_.chr_heads = (int)jnum(js, "chr", "heads", 6); cfg_.vocab = (int)jnum(js, "chr", "vocab", 14517); cfg_.chr_eps = (float)jnum(js, "chr", "ln_eps", 1e-5);
    // The attention kernel keeps a head in 64 registers and the add-and-norm kernel a row in 384 lanes.
    if (cfg_.chr_heads < 1 || cfg_.chr_d % cfg_.chr_heads != 0 || cfg_.chr_d / cfg_.chr_heads > 64 || cfg_.chr_d > 384)
        throw std::runtime_error("model.json: chr.d / chr.heads does not fit the attention kernel");
    cfg_.PAD = (int)jnum(js, "chr", "PAD", 0); cfg_.BOS = (int)jnum(js, "chr", "BOS", 1); cfg_.EOS = (int)jnum(js, "chr", "EOS", 2); cfg_.UNK = (int)jnum(js, "chr", "UNK", 3); cfg_.OFF = (int)jnum(js, "chr", "OFF", 4);
    // The frequency shortlist (after model.json, which gives OFF and vocab).  norm_rank.bin lists character indices -
    // index i is tables.json["chars"][i] - and a character's column in chr.emb is its index plus OFF.  Without the file
    // the normaliser runs over the whole vocabulary.
    {
        std::ifstream nf(dir + "/norm_rank.bin", std::ios::binary);
        if (nf) { int32_t n = 0; nf.read((char*)&n, 4); std::vector<int32_t> ids((size_t)std::max(0, n));
            if (n > 0) nf.read((char*)ids.data(), (std::streamsize)n * 4);
            if ((int)ids.size() > NORM_TOP) ids.resize(NORM_TOP);
            for (int32_t& c : ids) c += cfg_.OFF;
            norm_ids_ = std::move(ids); norm_in_.assign((size_t)cfg_.vocab, 0);
            for (int32_t c : norm_ids_) if (c >= 0 && c < cfg_.vocab) norm_in_[(size_t)c] = 1; }
    }
    cfg_.U = (int)jnum(js, "beam", "U", 256); cfg_.topk = (int)jnum(js, "beam", "topk", 48); cfg_.K = (int)jnum(js, "beam", "K", 256);
    cfg_.floor = (float)jnum(js, "beam", "floor", -12); cfg_.span_weight = (float)jnum(js, "beam", "span_weight", 0.5); cfg_.read_prior = (float)jnum(js, "beam", "read_prior", 0.25);
    cfg_.margin = (float)jnum(js, "beam", "margin", 12); cfg_.g_weight = (float)jnum(js, "beam", "g_weight", 1); cfg_.max_nodes = (long long)jnum(js, "beam", "max_nodes", 4000000);
    // No transposed copies of the weights: matmul takes a transposed view without a copy.  Layer-norm parameters are
    // stored in fp32 and cast once here, since a cast per call is two kernel launches per norm.
    { std::vector<std::string> names; for (auto& kv : w_) if (kv.second.dtype() == float32 && (kv.first.find("norm") != std::string::npos || kv.first.find(".ln") != std::string::npos || kv.first.find("emb_ln") != std::string::npos)) names.push_back(kv.first);
      for (auto& k : names) { array c = astype(w_.at(k), dt_); eval(c); w_.erase(k); w_.emplace(k, c); } }
    { std::vector<array> all; for (auto& kv : w_) all.push_back(kv.second); eval(all); }
    pos_rows_ = W("chr.pos").shape(0);   // the position table's length, read once
    clear_cache();
}
const array& V2Model::W(const std::string& k) const { auto it = w_.find(k); if (it == w_.end()) throw std::runtime_error("missing weight " + k); return it->second; }
array V2Model::linear(const array& x, const std::string& name) const {
    auto q = w_.find(name + ".weight.q");
    if (q != w_.end()) { const array& sc = W(name + ".weight.scales"); array y = quantized_matmul(astype(x, sc.dtype()), q->second, sc, W(name + ".weight.biases"), true, cfg_.quant_group, cfg_.quant_bits); return add(y, astype(W(name + ".bias"), y.dtype())); }
    const array& w = W(name + ".weight");
    if (w.dtype() == dt_) return addmm(W(name + ".bias"), x, transpose(w));                              // one kernel: x @ W^T + b (the transpose is a view)
    return add(matmul(astype(x, w.dtype()), transpose(w)), astype(W(name + ".bias"), w.dtype()));
}
array V2Model::ln(const array& x, const std::string& name, float eps) const { return fast::layer_norm(astype(x, float32), W(name + ".weight"), W(name + ".bias"), eps); }
array V2Model::ln16(const array& x, const std::string& name, float eps) const { return fast::layer_norm(x, astype(W(name + ".weight"), x.dtype()), astype(W(name + ".bias"), x.dtype()), eps); }
static array gelu(const array& x) { return aq_gelu_fused(x); }

// Paged single-query attention: one thread per (hypothesis, head) reads the hypothesis's context rows straight
// from the K/V tables by slot id (online softmax, fp32 accumulation) and writes its head of the output.  Replaces, per layer,
// two gathers, two concatenations, the reshapes/transposes and the fused attention call - ten kernels - with one.
static const std::string PAGED_SRC = R"(
    uint t = thread_position_in_grid.x; int n = dims[0], h = dims[1], hd = dims[2], L = dims[3], QS = dims[5]; if (t >= (uint)(n * h)) return;
    int r = t / h, hh = t % h, d = h * hd, len = lens[r]; float scale = fscale[0];
    float m = -1e30f, z = 0.0f; float acc[64]; for (int c = 0; c < hd; c++) acc[c] = 0.0f;
    const device half* qr = q + r * QS + hh * hd;                                   // q is the first d columns of the fused q|k|v projection (row stride QS)
    for (int j = 0; j < len; j++) {
        int slot = idx[r * L + j]; int pg = slot >> 10;   // PAGE = 1024: a shift and a mask, no integer division; a select chain, no pointer array in thread memory
        const device int8_t* base = pg == 0 ? p0 : pg == 1 ? p1 : pg == 2 ? p2 : pg == 3 ? p3 : pg == 4 ? p4 : pg == 5 ? p5 : pg == 6 ? p6 : pg == 7 ? p7 : pg == 8 ? p8 : pg == 9 ? p9 : pg == 10 ? p10 : pg == 11 ? p11 : pg == 12 ? p12 : pg == 13 ? p13 : pg == 14 ? p14 : pg == 15 ? p15 : pg == 16 ? p16 : pg == 17 ? p17 : pg == 18 ? p18 : pg == 19 ? p19 : pg == 20 ? p20 : pg == 21 ? p21 : pg == 22 ? p22 : p23;
        const device int8_t* rowp = base + (slot & 1023) * (2 * d + 4); const device int8_t* kr = rowp + hh * hd; const device int8_t* vr = kr + d;
        const device half* sp = (const device half*)(rowp + 2 * d); float ks = (float)sp[0], vs = (float)sp[1];
        float s = 0.0f; for (int c = 0; c < hd; c++) s += (float)qr[c] * (float)kr[c]; s *= scale * ks;
        float mn = metal::max(m, s); float corr = metal::exp(m - mn); float w = metal::exp(s - mn) * vs;
        z = z * corr + w / vs; for (int c = 0; c < hd; c++) acc[c] = acc[c] * corr + w * (float)vr[c]; m = mn;
    }
    for (int c = 0; c < hd; c++) out[r * d + hh * hd + c] = (half)(acc[c] / z);
)";
static array paged_attention(const array& q, int q_stride, const std::vector<array>& pages, const array& idx, const array& lens, int n, int h, int hd, int L) {
    static fast::CustomKernelFunction kern = fast::metal_kernel("aq_paged_attn", {"q", "p0", "p1", "p2", "p3", "p4", "p5", "p6", "p7", "p8", "p9", "p10", "p11", "p12", "p13", "p14", "p15", "p16", "p17", "p18", "p19", "p20", "p21", "p22", "p23", "idx", "lens", "dims", "fscale"}, {"out"}, PAGED_SRC);
    array dims({n, h, hd, L, V2Model::PAGE, q_stride}, {6}, int32); array sc({1.0f / std::sqrt((float)hd)}, {1}, float32);
    std::vector<array> in{q}; for (int p = 0; p < V2Model::NPAGES; p++) in.push_back(pages[p]); in.push_back(idx); in.push_back(lens); in.push_back(dims); in.push_back(sc);
    int total = n * h; return kern(in, {Shape{n, h * hd}}, {float16}, std::make_tuple(((total + 63) / 64) * 64, 1, 1), std::make_tuple(64, 1, 1), {}, std::nullopt, false, {})[0];
}
// write rows (row-major [n, 2d]) into the pages their slots fall in: one small scatter per touched page
static void write_rows(std::vector<array>& pages, const std::vector<int32_t>& slots, const array& rows, int d2) {
    std::map<int, std::vector<int>> by_page; for (size_t i = 0; i < slots.size(); i++) by_page[slots[i] / V2Model::PAGE].push_back((int)i);
    for (auto& [p, ii] : by_page) {
        std::vector<int32_t> ri(ii.size()), src(ii.size()); for (size_t k = 0; k < ii.size(); k++) { ri[k] = slots[ii[k]] % V2Model::PAGE; src[k] = ii[k]; }
        array sel = ii.size() == slots.size() ? rows : take(rows, array(src.data(), {(int)src.size()}, int32), 0);
        pages[p] = scatter(pages[p], {array(ri.data(), {(int)ri.size()}, int32)}, reshape(sel, {(int)ii.size(), 1, d2}), {0});
    }
}

// k | v rows [n, 2d] fp16 -> int8 rows [n, 2d + 4]: each half scaled by its absmax / 127, the two fp16 scales stored after the
// values.  One kernel launch (the unfused MLX ops cost ~30 ms a stop); the attention kernel multiplies the scales back.
// Measured metric-neutral against fp16 rows.
static const std::string QUANT_SRC = R"(
    uint t = thread_position_in_grid.x; int n = dims[0], d = dims[1], RS = dims[2], OFF = dims[3]; if (t >= (uint)(n * 2)) return;
    int r = t >> 1, hf = t & 1; const device half* src = kv + r * RS + OFF + hf * d; device int8_t* dst = out + r * (2 * d + 4) + hf * d;   // k | v live at columns OFF.. of a row of stride RS
    float mx = 0.0f; for (int c = 0; c < d; c++) mx = metal::max(mx, metal::fabs((float)src[c]));
    float s = metal::max(mx / 127.0f, 1e-6f), inv = 1.0f / s;
    for (int c = 0; c < d; c++) dst[c] = (int8_t)metal::rint((float)src[c] * inv);
    device half* sp = (device half*)(out + r * (2 * d + 4) + 2 * d); sp[hf] = (half)s;
)";
static array quant_rows(const array& kv, int n, int d, int row_stride, int col_off) {
    static fast::CustomKernelFunction kern = fast::metal_kernel("aq_quant_rows", {"kv", "dims"}, {"out"}, QUANT_SRC);
    array dims({n, d, row_stride, col_off}, {4}, int32); int total = n * 2;
    return kern({kv, dims}, {Shape{n, 2 * d + 4}}, {int8}, std::make_tuple(((total + 63) / 64) * 64, 1, 1), std::make_tuple(64, 1, 1), {}, std::nullopt, false, {})[0];
}

// x + b (the residual stream after a projection whose bias was not folded into the matmul), then layer norm of that: one
// launch instead of two or three (bias add, residual add, norm), both results returned - the stream for the next residual
// and the normalised input of the next block.  The sum is rounded to fp16 before the statistics, as separate kernels would.
static const std::string ADDLN_SRC = R"(
    uint r = threadgroup_position_in_grid.x; uint lane = thread_index_in_threadgroup; int n = dims[0], d = dims[1]; if (r >= (uint)n) return;
    float eps = feps[0]; const device half* xr = x + r * d; device half* xo_r = xo + r * d; device half* yr = y + r * d;
    threadgroup float sh[4];
    float v[3]; float s = 0.0f;
    for (int k = 0; k < 3; k++) { int c = (int)lane + k * 128; if (c < d) { half t = (half)((float)xr[c] + (float)b[c]); xo_r[c] = t; v[k] = (float)t; s += v[k]; } else v[k] = 0.0f; }
    s = simd_sum(s); if (thread_index_in_simdgroup == 0) sh[simdgroup_index_in_threadgroup] = s; threadgroup_barrier(mem_flags::mem_threadgroup);
    float mean = (sh[0] + sh[1] + sh[2] + sh[3]) / (float)d; threadgroup_barrier(mem_flags::mem_threadgroup);
    float q = 0.0f; for (int k = 0; k < 3; k++) { int c = (int)lane + k * 128; if (c < d) { float t = v[k] - mean; q += t * t; } }
    q = simd_sum(q); if (thread_index_in_simdgroup == 0) sh[simdgroup_index_in_threadgroup] = q; threadgroup_barrier(mem_flags::mem_threadgroup);
    float rstd = metal::rsqrt((sh[0] + sh[1] + sh[2] + sh[3]) / (float)d + eps);
    for (int k = 0; k < 3; k++) { int c = (int)lane + k * 128; if (c < d) yr[c] = (half)((v[k] - mean) * rstd * (float)g[c] + (float)be[c]); }
)";
static std::pair<array, array> add_ln(const array& x, const array& b, const array& g, const array& be, float eps) {
    static fast::CustomKernelFunction kern = fast::metal_kernel("aq_add_ln", {"x", "b", "g", "be", "dims", "feps"}, {"xo", "y"}, ADDLN_SRC);
    int n = x.shape(0), d = x.shape(1); array dims({n, d}, {2}, int32); array fe({eps}, {1}, float32);
    if (d > 384) throw std::runtime_error("add_ln: d > 384"); auto outs = kern({x, b, g, be, dims, fe}, {Shape{n, d}, Shape{n, d}}, {float16, float16}, std::make_tuple(n * 128, 1, 1), std::make_tuple(128, 1, 1), {}, std::nullopt, false, {});   // one 128-thread group per row, 3 elements a thread
    return {outs[0], outs[1]};
}

// ---------------------------------------------------------------- syllable model
array V2Model::syl_hidden(const std::vector<int32_t>& key_ids) const {
    const int L = (int)key_ids.size(), d = cfg_.syl_d, h = cfg_.syl_heads, hd = d / h;
    array ids(key_ids.data(), {L}, int32);
    array x = add(add(take(W("syl.key_emb"), ids, 0), slice(W("syl.pos"), {0, 0}, {L, d})), reshape(W("syl.tok0"), {1, d}));
    x = astype(ln(x, "syl.emb_ln", cfg_.syl_eps), dt_);                                         // [L, d]
    for (int i = 0; i < cfg_.syl_layers; i++) {
        std::string p = "syl.layers." + std::to_string(i) + ".";
        array q = transpose(reshape(linear(x, p + "q"), {1, L, h, hd}), {0, 2, 1, 3}), k = transpose(reshape(linear(x, p + "k"), {1, L, h, hd}), {0, 2, 1, 3}), v = transpose(reshape(linear(x, p + "v"), {1, L, h, hd}), {0, 2, 1, 3});
        array a = reshape(transpose(fast::scaled_dot_product_attention(q, k, v, 1.0f / std::sqrt((float)hd)), {0, 2, 1, 3}), {L, d});
        x = ln16(add(x, linear(a, p + "o")), p + "ln1", cfg_.syl_eps);
        x = ln16(add(x, linear(gelu(linear(x, p + "ff1")), p + "ff2")), p + "ln2", cfg_.syl_eps);
    }
    array hm = ln(gelu(linear(x, "syl.mlm_dense")), "syl.mlm_ln", cfg_.syl_eps);                 // fp32 [L, d]
    eval(hm); return hm;
}
void V2Model::compact(KVCache& c, const std::vector<int32_t>& keep) const {
    const int d = cfg_.chr_d, RS = 2 * d + 4, NL = cfg_.chr_layers, P = c.n_root; const int n_new = P + (int)keep.size(), need = (n_new + PAGE - 1) / PAGE;
    if (need > NPAGES) throw Overflow("v2: compaction needs more pages than the table has");
    array dummy = zeros({PAGE, RS}, int8);
    for (int i = 0; i < NL; i++) {
        std::vector<int8_t> buf((size_t)need * PAGE * RS, 0);
        auto src = [&](int slot) { return c.K[i][slot / PAGE].data<int8_t>() + (size_t)(slot % PAGE) * RS; };
        for (int j = 0; j < P; j++) memcpy(buf.data() + (size_t)j * RS, src(j), RS);
        for (size_t k = 0; k < keep.size(); k++) memcpy(buf.data() + (size_t)(P + k) * RS, src(keep[k]), RS);
        std::vector<array> pg(NPAGES, dummy); std::vector<array> fresh; for (int p = 0; p < need; p++) { pg[p] = array(buf.data() + (size_t)p * PAGE * RS, {PAGE, RS}, int8); fresh.push_back(pg[p]); }
        eval(fresh); c.K[i] = pg;
    }
    c.n_slots = n_new; c.n_pages = need;
}

// log P(character | all keys) at the given rows of the syllable model's hidden state, host copy [rows, n_char] fp32.
std::vector<float> V2Model::char_head_logits(const array& hfull, const std::vector<int32_t>& rows, const std::vector<int32_t>& cols) const {
    if (!has_char_head_ || rows.empty() || cols.empty()) return {};
    array idx(rows.data(), {(int)rows.size()}, int32), cid(cols.data(), {(int)cols.size()}, int32);
    array h = astype(take(hfull, idx, 0), dt_);                                              // [n, d]
    auto q = w_.find("syl.char_head.weight.q");
    array lg = [&] {
        if (q != w_.end()) {                                                                  // quantised: take the wanted rows of the weight, its scales and its biases
            array y = quantized_matmul(astype(h, W("syl.char_head.weight.scales").dtype()), take(q->second, cid, 0), take(W("syl.char_head.weight.scales"), cid, 0), take(W("syl.char_head.weight.biases"), cid, 0), true, cfg_.quant_group, cfg_.quant_bits);
            return add(y, astype(take(W("syl.char_head.bias"), cid, 0), y.dtype()));
        }
        const array& w = W("syl.char_head.weight");
        return add(matmul(astype(h, w.dtype()), transpose(take(w, cid, 0))), astype(take(W("syl.char_head.bias"), cid, 0), w.dtype()));
    }();
    lg = astype(lg, float32); eval(lg);
    return std::vector<float>(lg.data<float>(), lg.data<float>() + lg.size());
}
std::vector<float> V2Model::span_scores(const array& hfull, const std::vector<int32_t>& s, const std::vector<int32_t>& e) const {
    const int n = (int)s.size(); array si(s.data(), {n}, int32), ei(e.data(), {n}, int32);
    array feat = astype(concatenate({take(hfull, si, 0), take(hfull, ei, 0)}, 1), dt_);          // [n, 2d]
    array lg = add(astype(matmul(feat, transpose(W("syl.span.weight"))), float32), W("syl.span.bias"));
    array lp = subtract(lg, logsumexp(lg, -1, true)); eval(lp);
    return std::vector<float>(lp.data<float>(), lp.data<float>() + (size_t)lp.size());
}

// ---------------------------------------------------------------- character model with the tree KV cache
V2Model::KVCache V2Model::new_cache(const std::vector<int32_t>& root) const {
    KVCache c; const int d = cfg_.chr_d, h = cfg_.chr_heads, hd = d / h, L = (int)root.size(), NL = cfg_.chr_layers; c.cap = PAGE * NPAGES; c.n_root = L; c.n_slots = L; c.n_pages = 1;
    if (L > PAGE || L > pos_rows_) throw std::runtime_error("v2: context longer than the position table");
    array ids(root.data(), {L}, int32); array x = add(take(W("chr.emb"), ids, 0), slice(W("chr.pos"), {0, 0}, {L, d}));   // fp16 [L, d]
    array mask = triu(full({L, L}, NEG, dt_), 1); std::vector<array> ks, vs;
    for (int i = 0; i < NL; i++) {
        std::string p = "chr.layers." + std::to_string(i) + ".";
        array y = ln16(x, p + "norm1", cfg_.chr_eps); array qkv = addmm(W(p + "in_proj.bias"), y, transpose(W(p + "in_proj.weight")));
        array q = slice(qkv, {0, 0}, {L, d}), k = slice(qkv, {0, d}, {L, 2 * d}), v = slice(qkv, {0, 2 * d}, {L, 3 * d}); ks.push_back(k); vs.push_back(v);
        array a = reshape(transpose(fast::scaled_dot_product_attention(transpose(reshape(q, {1, L, h, hd}), {0, 2, 1, 3}), transpose(reshape(k, {1, L, h, hd}), {0, 2, 1, 3}), transpose(reshape(v, {1, L, h, hd}), {0, 2, 1, 3}), 1.0f / std::sqrt((float)hd), "", mask), {0, 2, 1, 3}), {L, d});
        x = add(x, linear(a, p + "out")); y = ln16(x, p + "norm2", cfg_.chr_eps); x = add(x, linear(gelu(linear(y, p + "l1")), p + "l2"));
    }
    const int RS = 2 * d + 4; array dummy = zeros({PAGE, RS}, int8);                           // pages not yet allocated share one zero page (a tiny dummy would land in the constant address space, which the kernel's pointer select cannot mix); the kernel never reads it
    for (int i = 0; i < NL; i++) { std::vector<array> pg(NPAGES, dummy); pg[0] = slice_update(zeros({PAGE, RS}, int8), quant_rows(contiguous(concatenate({ks[i], vs[i]}, 1)), L, d, 2 * d, 0), {0, 0}, {L, RS}); c.K.push_back(pg); }
    array hl = ln16(slice(x, {L - 1, 0}, {L, d}), "chr.ln", cfg_.chr_eps); array lg = matmul(hl, transpose(W("chr.emb")));   // fp16 logits; the normaliser accumulates in fp32 inside MLX's kernel
    array lp = subtract(astype(lg, float32), astype(logsumexp(lg, -1, true), float32)); eval(lp); for (auto& pg : c.K) eval(pg);
    c.root_lp.assign(lp.data<float>(), lp.data<float>() + (size_t)lp.size()); return c;
}

// The rows of chr.emb the normaliser runs over: the frequency shortlist plus any column this step is scoring that is not
// already in it, so no candidate is ever normalised by a sum that excludes itself.
mc::array V2Model::norm_emb(const std::vector<int32_t>& cols) const {
    if (!norm_emb_) { norm_emb_ = take(W("chr.emb"), mc::array(norm_ids_.data(), {(int)norm_ids_.size()}, mc::int32), 0); eval(*norm_emb_); }
    std::vector<int32_t> extra; extra.reserve(cols.size());
    for (int32_t c : cols) if (c >= 0 && c < (int)norm_in_.size() && !norm_in_[(size_t)c]) { extra.push_back(c); norm_in_[(size_t)c] = 2; }
    for (int32_t c : extra) norm_in_[(size_t)c] = 0;
    if (extra.empty()) return *norm_emb_;                                   // the shortlist alone, gathered once at first use
    return concatenate({*norm_emb_, take(W("chr.emb"), mc::array(extra.data(), {(int)extra.size()}, mc::int32), 0)}, 0);
}

V2Model::StepOut V2Model::step(KVCache& c, const std::vector<std::vector<int32_t>>& anc, int n_anc, const std::vector<int32_t>& tok, const std::vector<int32_t>& cols, std::vector<int32_t>& slots, const std::vector<int32_t>* reuse,
                               const std::vector<uint16_t>& part_hidden, const std::vector<float>& part_lse) const {
    const int n = n_anc, d = cfg_.chr_d, P = c.n_root, NL = cfg_.chr_layers;   // anc is reused across steps; only the first n_anc rows are this step's
    int n_new = 0; for (int i = 0; i < n; i++) if (!reuse || (*reuse)[i] < 0) n_new++;
    if (c.n_slots + n_new > c.cap) throw Overflow("v2: K/V table full");
    for (int p = c.n_slots / PAGE; n && p <= (c.n_slots + n_new - 1) / PAGE && p < NPAGES; p++) if (p >= c.n_pages) { for (int i = 0; i < NL; i++) c.K[i][p] = zeros({PAGE, 2 * d + 4}, int8); c.n_pages = p + 1; }   // allocate the pages this step reaches
    int L = 0; for (int i = 0; i < n; i++) L = std::max(L, P + (int)anc[i].size() + 1);                       // context rows: root + ancestors + the new token itself (its slot is written before the kernel reads)
    std::vector<int32_t> idx((size_t)n * L, 0), lens(n), depth(n); slots.resize(n);
    for (int i = 0; i < n; i++) {
        int t = 0; for (int r = 0; r < P; r++) idx[(size_t)i * L + t++] = r;
        for (int32_t s : anc[i]) idx[(size_t)i * L + t++] = s;
        // Past the position table the model is outside anything it was trained on, and a gather past the end would read
        // memory that is not the table: the last row stands in, so the scores stay defined.  Trimming the context
        // instead would have to be decided before the characters are known, and would cost ordinary long sentences
        // the context they fit with.
        slots[i] = (reuse && (*reuse)[i] >= 0) ? (*reuse)[i] : c.n_slots++; depth[i] = std::min(t, pos_rows_ - 1); idx[(size_t)i * L + t++] = slots[i]; lens[i] = t;
    }
    StepOut out; const int M = (int)cols.size(); const int m_part = (int)part_lse.size(); const int h = cfg_.chr_heads, hd = d / h;
    array colsa(cols.data(), {M}, int32); array embc = take(W("chr.emb"), colsa, 0);                                            // [M, d] fp16
    std::optional<array> part_lp;
    if (m_part > 0) {                                                                                                          // served prefixes: hidden . emb[cols] - normaliser
        array ph = view(array(part_hidden.data(), {m_part, cfg_.chr_d}, uint16), float16); array pl(part_lse.data(), {m_part, 1}, float32);   // the stored fp16 bits reinterpreted: a typed constructor would convert the integers
        part_lp = subtract(astype(matmul(ph, transpose(embc)), float32), pl);
    }
    if (n == 0) { eval(*part_lp); out.part_lp.assign(part_lp->data<float>(), part_lp->data<float>() + (size_t)part_lp->size()); return out; }
    array idx_m(idx.data(), {n * L}, int32), lens_m(lens.data(), {n}, int32);
    array x = add(take(W("chr.emb"), array(tok.data(), {n}, int32), 0), take(W("chr.pos"), array(depth.data(), {n}, int32), 0));   // [n, d]
    auto tb0 = std::chrono::steady_clock::now();
    array y = ln16(x, "chr.layers.0.norm1", cfg_.chr_eps);
    for (int i = 0; i < NL; i++) {
        std::string p = "chr.layers." + std::to_string(i) + ".";
        array qkv = addmm(W(p + "in_proj.bias"), y, transpose(W(p + "in_proj.weight")));                                       // [n, 3d]: q | k | v in one launch
        write_rows(c.K[i], slots, quant_rows(qkv, n, d, 3 * d, d), 2 * d + 4);
        array a = paged_attention(qkv, 3 * d, c.K[i], idx_m, lens_m, n, h, hd, L);
        x = addmm(x, a, transpose(W(p + "out.weight")));                                                                      // residual + projection in one launch; the bias goes into the norm kernel
        auto xy = add_ln(x, W(p + "out.bias"), W(p + "norm2.weight"), W(p + "norm2.bias"), cfg_.chr_eps); x = xy.first; y = xy.second;
        array g = gelu(addmm(W(p + "l1.bias"), y, transpose(W(p + "l1.weight"))));
        x = addmm(x, g, transpose(W(p + "l2.weight")));
        const std::string nn = (i + 1 < NL) ? "chr.layers." + std::to_string(i + 1) + ".norm1" : "chr.ln";                     // the next block's norm, or the final one
        xy = add_ln(x, W(p + "l2.bias"), W(nn + ".weight"), W(nn + ".bias"), cfg_.chr_eps); x = xy.first; y = xy.second;
    }
    const array& hl = y;
    array lse = astype(logsumexp(matmul(hl, transpose(!norm_ids_.empty() ? norm_emb(cols) : W("chr.emb"))), -1, true), float32);   // [n, V] or [n, shortlist u cols]
    array lp = subtract(astype(matmul(hl, transpose(embc)), float32), lse);                                                    // the requested columns from the same hidden state
    auto tb1 = std::chrono::steady_clock::now();
    if (part_lp) eval(lp, hl, lse, *part_lp); else eval(lp, hl, lse);
    for (auto& pg : c.K) eval(pg); auto tb2 = std::chrono::steady_clock::now();
    out.hidden.assign(hl.data<uint16_t>(), hl.data<uint16_t>() + (size_t)hl.size()); out.lse.assign(lse.data<float>(), lse.data<float>() + (size_t)lse.size());
    if (part_lp) out.part_lp.assign(part_lp->data<float>(), part_lp->data<float>() + (size_t)part_lp->size());
    prof_build_ms += std::chrono::duration<double, std::milli>(tb1 - tb0).count(); prof_eval_ms += std::chrono::duration<double, std::milli>(tb2 - tb1).count(); prof_steps++;
    out.lp.assign(lp.data<float>(), lp.data<float>() + (size_t)lp.size()); return out;
}
}  // namespace aq
