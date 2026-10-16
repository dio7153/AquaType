// The two v2 models on the CPU (see v2_model_cpu.h).  Each function mirrors its MLX counterpart in v2_model.cpp.
#include "v2_model_cpu.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <immintrin.h>
#include <omp.h>

namespace aq {
using namespace cpu;

// AQ_PROF: accumulate the character step's stages and print them when the process exits
static const bool PROF = std::getenv("AQ_PROF") != nullptr;
static double prof_t[12]; static long long prof_rows = 0;
static const char* prof_names[12] = {"setup", "in_proj", "kv_write", "attn", "out", "ln2", "l1", "gelu", "l2", "ln", "lse", "tail"};
struct ProfPrint { ~ProfPrint() { if (!PROF) return; double s = 0; for (double v : prof_t) s += v; std::fprintf(stderr, "[prof] rows %lld total %.0f ms:", prof_rows, s); for (int i = 0; i < 12; i++) std::fprintf(stderr, " %s %.0f", prof_names[i], prof_t[i]); std::fprintf(stderr, "\n"); } } static prof_print;
struct Lap { std::chrono::steady_clock::time_point t = std::chrono::steady_clock::now(); void operator()(int i) { if (!PROF) return; auto n = std::chrono::steady_clock::now(); prof_t[i] += std::chrono::duration<double, std::milli>(n - t).count(); t = n; } };
static std::string read_file(const std::string& p) { std::ifstream f(p); if (!f) throw std::runtime_error("cannot read " + p); std::stringstream ss; ss << f.rdbuf(); return ss.str(); }
static double jnum(const std::string& js, const std::string& sec, const std::string& key, double dflt) {   // "sec": { ... "key": number ... }
    size_t sp = sec.empty() ? 0 : js.find("\"" + sec + "\""); if (sp == std::string::npos) return dflt;
    size_t pos = js.find("\"" + key + "\"", sp); if (pos == std::string::npos) return dflt; pos = js.find(':', pos); if (pos == std::string::npos) return dflt;
    size_t i = pos + 1; while (i < js.size() && js[i] == ' ') i++; if (js.compare(i, 4, "null") == 0) return dflt; return std::atof(js.c_str() + i);
}

std::vector<float> V2Model::F(const std::string& k) const { auto it = w_.find(k); if (it == w_.end()) throw std::runtime_error("missing weight " + k); return to_f32(it->second); }
V2Model::Lin V2Model::lin(const std::string& name) const {
    Lin L; L.b = F(name + ".bias");
    auto q = w_.find(name + ".weight.q");
    if (q != w_.end()) {
        L.quant = true; L.q.keep = q->second; L.q.q = q->second.data<uint32_t>(); L.q.bits = cfg_.quant_bits; L.q.group = cfg_.quant_group;
        L.q.out = q->second.shape(0); L.q.in = q->second.shape(1) * (32 / cfg_.quant_bits);
        L.q.scales = F(name + ".weight.scales"); L.q.biases = F(name + ".weight.biases"); L.out = L.q.out; L.in = L.q.in;
        if ((int)L.q.scales.size() != L.out * (L.in / L.q.group)) throw std::runtime_error("quantised weight " + name + ": scales do not match");
    } else { auto it = w_.find(name + ".weight"); if (it == w_.end()) throw std::runtime_error("missing weight " + name);
        L.w = to_f32(it->second); L.out = it->second.shape(0); L.in = it->second.shape(1); L.pk = pack_nt(L.w.data(), L.out, L.in); }
    return L;
}
void V2Model::apply(const Lin& L, const float* x, int n, float* y, bool acc) const {
    if (L.quant) { if (acc) throw std::logic_error("apply: accumulate on a quantised weight"); gemm_q(x, n, L.q, y, L.b.data()); }
    else if (L.i8) gemm_i8(x, n, L.w8, y, L.b.data(), acc);
    else gemm_packed(x, n, L.pk, y, L.b.data(), acc);
}

V2Model::V2Model(const std::string& dir) {
    { auto loaded = load_safetensors(dir + "/model.safetensors").first; w_.insert(loaded.begin(), loaded.end()); }
    has_char_head_ = w_.count("syl.char_head.weight.q") || w_.count("syl.char_head.weight");
    std::string js = read_file(dir + "/model.json");
    cfg_.syl_d = (int)jnum(js, "syl", "d", 1024); cfg_.syl_layers = (int)jnum(js, "syl", "layers", 12); cfg_.syl_heads = (int)jnum(js, "syl", "heads", 16); cfg_.n_syl = (int)jnum(js, "syl", "n_syl", 1784); cfg_.n_keys = (int)jnum(js, "syl", "n_keys", 44); cfg_.syl_eps = (float)jnum(js, "syl", "ln_eps", 1e-12);
    n_char_ = (int)jnum(js, "syl", "n_char", 0);
    if (js.find("\"quant\": {") != std::string::npos) { cfg_.quant_bits = (int)jnum(js, "quant", "bits", 6); cfg_.quant_group = (int)jnum(js, "quant", "group_size", 64); }
    if (cfg_.quant_bits && 32 % cfg_.quant_bits != 0) throw std::runtime_error("model.json: unsupported quantisation bits");
    cfg_.chr_d = (int)jnum(js, "chr", "d", 384); cfg_.chr_layers = (int)jnum(js, "chr", "layers", 6); cfg_.chr_heads = (int)jnum(js, "chr", "heads", 6); cfg_.vocab = (int)jnum(js, "chr", "vocab", 14517); cfg_.chr_eps = (float)jnum(js, "chr", "ln_eps", 1e-5);
    if (cfg_.chr_heads < 1 || cfg_.chr_d % cfg_.chr_heads != 0) throw std::runtime_error("model.json: chr.d is not a multiple of chr.heads");
    cfg_.PAD = (int)jnum(js, "chr", "PAD", 0); cfg_.BOS = (int)jnum(js, "chr", "BOS", 1); cfg_.EOS = (int)jnum(js, "chr", "EOS", 2); cfg_.UNK = (int)jnum(js, "chr", "UNK", 3); cfg_.OFF = (int)jnum(js, "chr", "OFF", 4);
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

    for (int i = 0; i < cfg_.syl_layers; i++) { std::string p = "syl.layers." + std::to_string(i) + "."; SylLayer L;
        L.q = lin(p + "q"); L.k = lin(p + "k"); L.v = lin(p + "v"); L.o = lin(p + "o"); L.ff1 = lin(p + "ff1"); L.ff2 = lin(p + "ff2");
        L.ln1g = F(p + "ln1.weight"); L.ln1b = F(p + "ln1.bias"); L.ln2g = F(p + "ln2.weight"); L.ln2b = F(p + "ln2.bias"); syl_.push_back(std::move(L)); }
    key_emb_ = F("syl.key_emb"); syl_pos_ = F("syl.pos"); tok0_ = F("syl.tok0"); emb_lng_ = F("syl.emb_ln.weight"); emb_lnb_ = F("syl.emb_ln.bias");
    mlm_dense_ = lin("syl.mlm_dense"); mlm_lng_ = F("syl.mlm_ln.weight"); mlm_lnb_ = F("syl.mlm_ln.bias");
    { std::vector<float> sw = F("syl.span.weight"); span_b_ = F("syl.span.bias"); span_pk_ = pack_nt(sw.data(), (int)span_b_.size(), (int)(sw.size() / span_b_.size())); }
    if (has_char_head_) char_head_ = lin("syl.char_head");
    for (int i = 0; i < cfg_.chr_layers; i++) { std::string p = "chr.layers." + std::to_string(i) + "."; ChrLayer L;
        L.in_proj = lin(p + "in_proj"); L.out = lin(p + "out"); L.l1 = lin(p + "l1"); L.l2 = lin(p + "l2");
        L.n1g = F(p + "norm1.weight"); L.n1b = F(p + "norm1.bias"); L.n2g = F(p + "norm2.weight"); L.n2b = F(p + "norm2.bias");
        // AQ_CHR_I8 (default on; 0 = fp32): the character model's weights as int8, a quarter of the packed fp32's memory
        static const bool chr_i8 = [] { const char* e = std::getenv("AQ_CHR_I8"); return !(e && std::string(e) == "0"); }();
        for (Lin* l : {&L.in_proj, &L.out, &L.l1, &L.l2}) if (chr_i8 && !l->quant && l->in % 64 == 0) { l->w8 = quant_weight_i8(l->w.data(), l->out, l->in); l->i8 = true; l->pk = Packed(); }
        for (Lin* l : {&L.in_proj, &L.out, &L.l1, &L.l2}) std::vector<float>().swap(l->w);    // packed (or int8) copies only
        chr_.push_back(std::move(L)); }
    for (SylLayer& L : syl_) for (Lin* l : {&L.q, &L.k, &L.v, &L.o, &L.ff1, &L.ff2}) std::vector<float>().swap(l->w);
    std::vector<float>().swap(mlm_dense_.w);
    chr_emb_ = F("chr.emb"); chr_pos_ = F("chr.pos"); chr_lng_ = F("chr.ln.weight"); chr_lnb_ = F("chr.ln.bias");
    pos_rows_ = w_.at("chr.pos").shape(0);
    const int d = cfg_.chr_d;
    if (!norm_ids_.empty()) { std::vector<float> ne(norm_ids_.size() * (size_t)d); for (size_t i = 0; i < norm_ids_.size(); i++) std::memcpy(ne.data() + i * d, chr_emb_.data() + (size_t)norm_ids_[i] * d, (size_t)d * 4);
        norm_pk_ = pack_nt(ne.data(), (int)norm_ids_.size(), d); }
    // Only the packed 4-bit weights are read from the file's buffer from here on; the fp16 tensors were widened.
    for (auto it = w_.begin(); it != w_.end();) { if (it->first.size() > 2 && it->first.compare(it->first.size() - 2, 2, ".q") == 0) ++it; else it = w_.erase(it); }
}

// ---------------------------------------------------------------- syllable model
// bidirectional attention over L rows: q, k, v [L, d] -> out [L, d]
static void full_attention(const float* q, const float* k, const float* v, int L, int h, int hd, float* out) {
    const int d = h * hd; const float scale = 1.0f / std::sqrt((float)hd);
    #pragma omp parallel for num_threads(num_threads()) if (L >= 8)
    for (int t = 0; t < L * h; t++) { const int r = t / h, hh = t % h;
        std::vector<float> s(L); float mx = -INFINITY; const float* qr = q + (size_t)r * d + hh * hd;
        for (int j = 0; j < L; j++) { const float* kr = k + (size_t)j * d + hh * hd; float a = 0; for (int c = 0; c < hd; c++) a += qr[c] * kr[c]; s[j] = a * scale; mx = std::max(mx, s[j]); }
        float z = 0; for (int j = 0; j < L; j++) { s[j] = std::exp(s[j] - mx); z += s[j]; }
        float* o = out + (size_t)r * d + hh * hd; for (int c = 0; c < hd; c++) o[c] = 0;
        for (int j = 0; j < L; j++) { const float w = s[j] / z; const float* vr = v + (size_t)j * d + hh * hd; for (int c = 0; c < hd; c++) o[c] += w * vr[c]; } }
}

array V2Model::syl_hidden(const std::vector<int32_t>& key_ids) const {
    const int L = (int)key_ids.size(), d = cfg_.syl_d, h = cfg_.syl_heads, hd = d / h, ff = syl_.empty() ? 0 : syl_[0].ff1.out;
    if (L > (int)(syl_pos_.size() / d)) throw std::runtime_error("v2: key string longer than the syllable model's position table");
    std::vector<float> x((size_t)L * d), q(x.size()), k(x.size()), v(x.size()), a(x.size()), t(x.size()), f((size_t)L * ff);
    for (int i = 0; i < L; i++) { const int id = key_ids[i]; if (id < 0 || (size_t)(id + 1) * d > key_emb_.size()) throw std::runtime_error("v2: key id out of range");
        for (int c = 0; c < d; c++) x[(size_t)i * d + c] = key_emb_[(size_t)id * d + c] + syl_pos_[(size_t)i * d + c] + tok0_[c]; }
    layer_norm(x.data(), L, d, emb_lng_.data(), emb_lnb_.data(), cfg_.syl_eps, x.data());
    for (const SylLayer& P : syl_) {
        if (stop && stop->load(std::memory_order_relaxed)) return array(Dtype::f32, {0, d});
        apply(P.q, x.data(), L, q.data()); apply(P.k, x.data(), L, k.data()); apply(P.v, x.data(), L, v.data());
        full_attention(q.data(), k.data(), v.data(), L, h, hd, a.data());
        apply(P.o, a.data(), L, t.data()); for (size_t i = 0; i < x.size(); i++) t[i] += x[i];
        layer_norm(t.data(), L, d, P.ln1g.data(), P.ln1b.data(), cfg_.syl_eps, x.data());
        apply(P.ff1, x.data(), L, f.data()); gelu_inplace(f.data(), f.size());
        apply(P.ff2, f.data(), L, t.data()); for (size_t i = 0; i < x.size(); i++) t[i] += x[i];
        layer_norm(t.data(), L, d, P.ln2g.data(), P.ln2b.data(), cfg_.syl_eps, x.data());
    }
    apply(mlm_dense_, x.data(), L, t.data()); gelu_inplace(t.data(), t.size());
    array hm(Dtype::f32, {L, d}); layer_norm(t.data(), L, d, mlm_lng_.data(), mlm_lnb_.data(), cfg_.syl_eps, hm.data<float>());
    return hm;
}

std::vector<float> V2Model::char_head_logits(const array& hfull, const std::vector<int32_t>& rows, const std::vector<int32_t>& cols) const {
    if (!has_char_head_ || rows.empty() || cols.empty()) return {};
    const int d = cfg_.syl_d; std::vector<float> h(rows.size() * (size_t)d);
    for (size_t i = 0; i < rows.size(); i++) std::memcpy(h.data() + i * d, hfull.data<float>() + (size_t)rows[i] * d, (size_t)d * 4);
    std::vector<float> out(rows.size() * cols.size());
    if (char_head_.quant) gemm_q(h.data(), (int)rows.size(), char_head_.q, out.data(), char_head_.b.data(), &cols);
    else { std::vector<float> w(cols.size() * (size_t)d), b(cols.size());
        for (size_t j = 0; j < cols.size(); j++) { std::memcpy(w.data() + j * d, char_head_.w.data() + (size_t)cols[j] * d, (size_t)d * 4); b[j] = char_head_.b[cols[j]]; }
        gemm_nt(h.data(), (int)rows.size(), d, w.data(), (int)cols.size(), out.data(), b.data()); }
    return out;
}
std::vector<float> V2Model::span_scores(const array& hfull, const std::vector<int32_t>& s, const std::vector<int32_t>& e) const {
    const int n = (int)s.size(), d = cfg_.syl_d, NS = (int)span_b_.size(); if (n == 0) return {};
    std::vector<float> feat((size_t)n * 2 * d); const float* h = hfull.data<float>();
    for (int i = 0; i < n; i++) { std::memcpy(feat.data() + (size_t)i * 2 * d, h + (size_t)s[i] * d, (size_t)d * 4); std::memcpy(feat.data() + (size_t)i * 2 * d + d, h + (size_t)e[i] * d, (size_t)d * 4); }
    std::vector<float> lg((size_t)n * NS); gemm_packed(feat.data(), n, span_pk_, lg.data(), span_b_.data());
    log_softmax_rows(lg.data(), n, NS); return lg;
}

// ---------------------------------------------------------------- character model with the tree K/V table
// one k | v row [2d] fp32 -> int8 k | int8 v | float k scale | float v scale
void V2Model::chr_layer_kv_row(const float* kv, int d, int8_t* dst) const {
    float sc[2] = {quant_i8(kv, d, dst), quant_i8(kv + d, d, dst + d)};
    std::memcpy(dst + 2 * d, sc, 8);
}

V2Model::KVCache V2Model::new_cache(const std::vector<int32_t>& root) const {
    KVCache c; const int d = cfg_.chr_d, h = cfg_.chr_heads, hd = d / h, L = (int)root.size(), NL = cfg_.chr_layers, RB = row_bytes();
    c.cap = PAGE * NPAGES; c.n_root = L; c.n_slots = L; c.n_pages = 1;
    if (L > PAGE || L > pos_rows_) throw std::runtime_error("v2: context longer than the position table");
    std::vector<float> x((size_t)L * d), y(x.size()), qkv((size_t)L * 3 * d), a(x.size()), g((size_t)L * (chr_.empty() ? 0 : chr_[0].l1.out));
    for (int i = 0; i < L; i++) for (int k = 0; k < d; k++) x[(size_t)i * d + k] = chr_emb_[(size_t)root[i] * d + k] + chr_pos_[(size_t)i * d + k];
    const float scale = 1.0f / std::sqrt((float)hd);
    c.K.assign(NL, {});
    for (int li = 0; li < NL; li++) { const ChrLayer& P = chr_[li];
        layer_norm(x.data(), L, d, P.n1g.data(), P.n1b.data(), cfg_.chr_eps, y.data());
        apply(P.in_proj, y.data(), L, qkv.data());
        c.K[li].resize((size_t)L * RB);
        for (int i = 0; i < L; i++) chr_layer_kv_row(qkv.data() + (size_t)i * 3 * d + d, d, c.K[li].data() + (size_t)i * RB);
        for (int r = 0; r < L; r++) for (int hh = 0; hh < h; hh++) {                         // causal attention in fp32 (the root's own rows are not read back quantised)
            std::vector<float> s(r + 1); float mx = -INFINITY; const float* qr = qkv.data() + (size_t)r * 3 * d + hh * hd;
            for (int j = 0; j <= r; j++) { const float* kr = qkv.data() + (size_t)j * 3 * d + d + hh * hd; float t = 0; for (int cc = 0; cc < hd; cc++) t += qr[cc] * kr[cc]; s[j] = t * scale; mx = std::max(mx, s[j]); }
            float z = 0; for (int j = 0; j <= r; j++) { s[j] = std::exp(s[j] - mx); z += s[j]; }
            float* o = a.data() + (size_t)r * d + hh * hd; for (int cc = 0; cc < hd; cc++) o[cc] = 0;
            for (int j = 0; j <= r; j++) { const float* vr = qkv.data() + (size_t)j * 3 * d + 2 * d + hh * hd; for (int cc = 0; cc < hd; cc++) o[cc] += s[j] / z * vr[cc]; } }
        apply(P.out, a.data(), L, x.data(), true);
        layer_norm(x.data(), L, d, P.n2g.data(), P.n2b.data(), cfg_.chr_eps, y.data());
        apply(P.l1, y.data(), L, g.data()); gelu_inplace(g.data(), g.size());
        apply(P.l2, g.data(), L, x.data(), true);
    }
    std::vector<float> hl(d); layer_norm(x.data() + (size_t)(L - 1) * d, 1, d, chr_lng_.data(), chr_lnb_.data(), cfg_.chr_eps, hl.data());
    c.root_lp.resize(cfg_.vocab); gemm_nt(hl.data(), 1, d, chr_emb_.data(), cfg_.vocab, c.root_lp.data());
    log_softmax_rows(c.root_lp.data(), 1, cfg_.vocab);
    return c;
}

void V2Model::compact(KVCache& c, const std::vector<int32_t>& keep) const {
    const int RB = row_bytes(), P = c.n_root; const int n_new = P + (int)keep.size(), need = (n_new + PAGE - 1) / PAGE;
    if (need > NPAGES) throw Overflow("v2: compaction needs more pages than the table has");
    for (auto& layer : c.K) { std::vector<int8_t> nb((size_t)n_new * RB);
        std::memcpy(nb.data(), layer.data(), (size_t)P * RB);
        for (size_t k = 0; k < keep.size(); k++) std::memcpy(nb.data() + (size_t)(P + k) * RB, layer.data() + (size_t)keep[k] * RB, RB);
        layer.swap(nb); }
    c.n_slots = n_new; c.n_pages = need;
}

V2Model::StepOut V2Model::step(KVCache& c, const std::vector<std::vector<int32_t>>& anc, int n_anc, const std::vector<int32_t>& tok, const std::vector<int32_t>& cols, std::vector<int32_t>& slots, const std::vector<int32_t>* reuse,
                               const std::vector<uint16_t>& part_hidden, const std::vector<float>& part_lse) const {
    const int n = n_anc, d = cfg_.chr_d, P = c.n_root, NL = cfg_.chr_layers, RB = row_bytes(), h = cfg_.chr_heads, hd = d / h;
    int n_new = 0; for (int i = 0; i < n; i++) if (!reuse || (*reuse)[i] < 0) n_new++;
    if (c.n_slots + n_new > c.cap) throw Overflow("v2: K/V table full");
    int L = 0; for (int i = 0; i < n; i++) L = std::max(L, P + (int)anc[i].size() + 1);
    std::vector<int32_t> idx((size_t)n * L, 0), lens(n), depth(n); slots.resize(n);
    for (int i = 0; i < n; i++) {
        int t = 0; for (int r = 0; r < P; r++) idx[(size_t)i * L + t++] = r;
        for (int32_t s : anc[i]) idx[(size_t)i * L + t++] = s;
        slots[i] = (reuse && (*reuse)[i] >= 0) ? (*reuse)[i] : c.n_slots++; depth[i] = std::min(t, pos_rows_ - 1); idx[(size_t)i * L + t++] = slots[i]; lens[i] = t;
    }
    for (auto& layer : c.K) if ((int)(layer.size() / RB) < c.n_slots) layer.resize((size_t)c.n_slots * RB);
    c.n_pages = (c.n_slots + PAGE - 1) / PAGE;
    StepOut out; const int M = (int)cols.size(), m_part = (int)part_lse.size();
    std::vector<float> embc((size_t)M * d); for (int j = 0; j < M; j++) std::memcpy(embc.data() + (size_t)j * d, chr_emb_.data() + (size_t)cols[j] * d, (size_t)d * 4);
    if (m_part > 0) {                                                                    // served prefixes: hidden . emb[cols] - normaliser
        std::vector<float> ph((size_t)m_part * d); h2f_n(part_hidden.data(), ph.data(), ph.size());
        out.part_lp.resize((size_t)m_part * M); gemm_nt(ph.data(), m_part, d, embc.data(), M, out.part_lp.data());
        for (int i = 0; i < m_part; i++) for (int j = 0; j < M; j++) out.part_lp[(size_t)i * M + j] -= part_lse[i];
    }
    if (n == 0) return out;
    auto tb0 = std::chrono::steady_clock::now(); Lap lap; prof_rows += n;
    const int FF = chr_[0].l1.out;
    std::vector<float> x((size_t)n * d), y(x.size()), qkv((size_t)n * 3 * d), a(x.size()), g((size_t)n * FF);
    for (int i = 0; i < n; i++) for (int k = 0; k < d; k++) x[(size_t)i * d + k] = chr_emb_[(size_t)tok[i] * d + k] + chr_pos_[(size_t)depth[i] * d + k];
    const float scale = 1.0f / std::sqrt((float)hd);
    layer_norm(x.data(), n, d, chr_[0].n1g.data(), chr_[0].n1b.data(), cfg_.chr_eps, y.data()); lap(0);
    for (int li = 0; li < NL; li++) { const ChrLayer& Pl = chr_[li];
        apply(Pl.in_proj, y.data(), n, qkv.data()); lap(1);
        int8_t* tab = c.K[li].data();
        #pragma omp parallel for num_threads(num_threads()) if (n >= 64)
        for (int i = 0; i < n; i++) chr_layer_kv_row(qkv.data() + (size_t)i * 3 * d + d, d, tab + (size_t)slots[i] * RB);
        lap(2);
        // paged attention: each (hypothesis, head) reads its context rows by slot, online softmax in fp32
        #pragma omp parallel for schedule(dynamic, 32) num_threads(num_threads()) if (n >= 16)
        for (int t = 0; t < n * h; t++) { const int r = t / h, hh = t % h, len = lens[r];
            const float* qr = qkv.data() + (size_t)r * 3 * d + hh * hd; float m = -1e30f, z = 0; float acc[256];
            for (int cc = 0; cc < hd; cc++) acc[cc] = 0;
            for (int j = 0; j < len; j++) { const int8_t* row = tab + (size_t)idx[(size_t)r * L + j] * RB; float sc[2]; std::memcpy(sc, row + 2 * d, 8);
                const int8_t* kr = row + hh * hd; const int8_t* vr = row + d + hh * hd;
                const float s = dot_f32_i8(qr, kr, hd) * scale * sc[0];
                const float mn = std::max(m, s), corr = std::exp(m - mn), w = std::exp(s - mn);
                z = z * corr + w; scale_add_i8(acc, corr, w * sc[1], vr, hd); m = mn; }
            float* o = a.data() + (size_t)r * d + hh * hd; for (int cc = 0; cc < hd; cc++) o[cc] = acc[cc] / z; }
        lap(3);
        apply(Pl.out, a.data(), n, x.data(), true); lap(4);
        layer_norm(x.data(), n, d, Pl.n2g.data(), Pl.n2b.data(), cfg_.chr_eps, y.data()); lap(5);
        apply(Pl.l1, y.data(), n, g.data()); lap(6); gelu_inplace(g.data(), g.size()); lap(7);
        apply(Pl.l2, g.data(), n, x.data(), true); lap(8);
        if (li + 1 < NL) layer_norm(x.data(), n, d, chr_[li + 1].n1g.data(), chr_[li + 1].n1b.data(), cfg_.chr_eps, y.data());
        else layer_norm(x.data(), n, d, chr_lng_.data(), chr_lnb_.data(), cfg_.chr_eps, y.data());
        lap(9);
    }
    const std::vector<float>& hl = y;
    // log P(col | prefix) = hl . emb[col] - lse; the normaliser over the frequency shortlist plus any requested column not in it
    out.lp.resize((size_t)n * M); gemm_nt(hl.data(), n, d, embc.data(), M, out.lp.data());
    out.lse.resize(n);
    if (!norm_ids_.empty()) {
        std::vector<int> extra; for (int j = 0; j < M; j++) { const int cidx = cols[j]; if (cidx >= 0 && cidx < (int)norm_in_.size() && !norm_in_[(size_t)cidx]) extra.push_back(j); }
        std::sort(extra.begin(), extra.end(), [&](int p, int q) { return cols[p] < cols[q]; });
        extra.erase(std::unique(extra.begin(), extra.end(), [&](int p, int q) { return cols[p] == cols[q]; }), extra.end());
        const int S = (int)norm_ids_.size(); std::vector<float> sl((size_t)n * (S + extra.size()));
        std::vector<float> shortl((size_t)n * S); gemm_packed(hl.data(), n, norm_pk_, shortl.data());
        #pragma omp parallel for num_threads(num_threads()) if (n >= 16)
        for (int i = 0; i < n; i++) { float* row = sl.data() + (size_t)i * (S + extra.size()); std::memcpy(row, shortl.data() + (size_t)i * S, (size_t)S * 4);
            for (size_t e = 0; e < extra.size(); e++) row[S + e] = out.lp[(size_t)i * M + extra[e]];
            out.lse[i] = logsumexp_row(row, S + (int)extra.size()); }
    } else {
        std::vector<float> full((size_t)n * cfg_.vocab); gemm_nt(hl.data(), n, d, chr_emb_.data(), cfg_.vocab, full.data());
        for (int i = 0; i < n; i++) out.lse[i] = logsumexp_row(full.data() + (size_t)i * cfg_.vocab, cfg_.vocab);
    }
    lap(10);
    for (int i = 0; i < n; i++) for (int j = 0; j < M; j++) out.lp[(size_t)i * M + j] -= out.lse[i];
    out.hidden.resize((size_t)n * d); f2h_n(hl.data(), out.hidden.data(), out.hidden.size()); lap(11);
    auto tb1 = std::chrono::steady_clock::now();
    prof_eval_ms += std::chrono::duration<double, std::milli>(tb1 - tb0).count(); prof_steps++;
    return out;
}
}  // namespace aq
