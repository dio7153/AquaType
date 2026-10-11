// The two v2 models on the CPU (AQ_CPU builds): the same interface as the MLX V2Model in v2_model.h, so v2_session.cpp
// runs unchanged.  Numerics follow the MLX path's formulas with fp32 activations throughout (the MLX path keeps the
// residual stream in fp16); the syllable model's linears use the 4-bit weights as stored, the character model's fp16
// weights are widened to fp32 once at load.  The K/V table keeps the MLX path's format: each row's k and v in 8 bits,
// each half scaled by its absmax / 127.
#pragma once
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
#include "cpu_ops.h"

namespace aq {

struct V2Config {
    int syl_d = 1024, syl_layers = 12, syl_heads = 16, n_syl = 1784, n_keys = 43, quant_bits = 0, quant_group = 64; float syl_eps = 1e-12f;
    int chr_d = 384, chr_layers = 6, chr_heads = 6, vocab = 14517, PAD = 0, BOS = 1, EOS = 2, UNK = 3, OFF = 4; float chr_eps = 1e-5f;
    int U = 256, topk = 64, K = 256; float floor = -12.f, span_weight = 0.5f, read_prior = 0.25f, margin = 12.f, g_weight = 1.f; long long max_nodes = 4000000;
};

class V2Model {
public:
    explicit V2Model(const std::string& dir);
    const V2Config& config() const { return cfg_; }
    // the syllable model's final hidden states [L, d] fp32
    cpu::array syl_hidden(const std::vector<int32_t>& key_ids) const;
    bool has_char_head() const { return has_char_head_; }
    int n_char() const { return n_char_; }
    std::vector<float> char_head_logits(const cpu::array& hfull, const std::vector<int32_t>& rows, const std::vector<int32_t>& cols) const;
    std::vector<float> span_scores(const cpu::array& h, const std::vector<int32_t>& s, const std::vector<int32_t>& e) const;

    // The character model's K/V table: per layer one growing buffer of rows (k | v in int8, then the two scales).
    // cap matches the MLX table's slot count, so the session's compaction and overflow rules behave the same.
    static constexpr int PAGE = 1024, NPAGES = 24;
    struct KVCache {
        std::vector<std::vector<int8_t>> K;     // [layer] -> n_slots * row_bytes
        int cap = 0, n_slots = 0, n_root = 0, n_pages = 0;
        std::vector<float> root_lp;
    };
    KVCache new_cache(const std::vector<int32_t>& root_tokens) const;
    void compact(KVCache& c, const std::vector<int32_t>& keep_old_slots) const;
    struct Overflow : std::runtime_error { using std::runtime_error::runtime_error; };
    struct StepOut { std::vector<float> lp, part_lp, lse; std::vector<uint16_t> hidden; };
    StepOut step(KVCache& c, const std::vector<std::vector<int32_t>>& anc_slots, int n_anc, const std::vector<int32_t>& tok, const std::vector<int32_t>& cols, std::vector<int32_t>& slots, const std::vector<int32_t>* reuse,
                 const std::vector<uint16_t>& part_hidden, const std::vector<float>& part_lse) const;
    mutable double prof_build_ms = 0, prof_eval_ms = 0; mutable long prof_steps = 0;
private:
    struct Lin { cpu::QWeight q; cpu::Packed pk; std::vector<float> w, b; int out = 0, in = 0; bool quant = false; };   // quantised or packed fp32 weight (w kept only where rows are picked from it), fp32 bias
    struct SylLayer { Lin q, k, v, o, ff1, ff2; std::vector<float> ln1g, ln1b, ln2g, ln2b; };
    struct ChrLayer { Lin in_proj, out, l1, l2; std::vector<float> n1g, n1b, n2g, n2b; };
    Lin lin(const std::string& name) const;
    void apply(const Lin& L, const float* x, int n, float* y, bool accumulate = false) const;   // y = x W^T + b (+ y)
    std::vector<float> F(const std::string& k) const;
    void chr_layer_kv_row(const float* kv, int d, int8_t* dst) const;
    int row_bytes() const { return 2 * cfg_.chr_d + 8; }

    V2Config cfg_; std::unordered_map<std::string, cpu::array> w_;
    std::vector<SylLayer> syl_; std::vector<float> key_emb_, syl_pos_, tok0_, emb_lng_, emb_lnb_, mlm_lng_, mlm_lnb_, span_b_; cpu::Packed span_pk_; Lin mlm_dense_, char_head_;
    std::vector<ChrLayer> chr_; std::vector<float> chr_emb_, chr_pos_, chr_lng_, chr_lnb_;
    static constexpr int NORM_TOP = 2048;
    std::vector<int32_t> norm_ids_; std::vector<char> norm_in_; cpu::Packed norm_pk_;
    bool has_char_head_ = false; int n_char_ = 0; int pos_rows_ = 0;
};
}  // namespace aq
