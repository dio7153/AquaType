// The two v2 models in C++ on MLX: the syllable model (key embedding + bidirectional encoder, linear weights quantised
// to the bits model.json gives - 4 in the shipped model - with the span head and the character head) and the character
// model (pre-LN GPT, fp16) with the tree-structured key/value table the beam needs.  Weights from
// decoder/v2/export_v2.py (model.safetensors + model.json).
#pragma once
#ifdef AQ_CPU
#include "cpu/v2_model_cpu.h"   // the same interface on the CPU, without MLX
#else
#include <optional>
#include <stdexcept>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "mlx/mlx.h"

namespace aq {
namespace mc = mlx::core;

struct V2Config {
    int syl_d = 1024, syl_layers = 12, syl_heads = 16, n_syl = 1784, n_keys = 43, quant_bits = 0, quant_group = 64; float syl_eps = 1e-12f;
    int chr_d = 384, chr_layers = 6, chr_heads = 6, vocab = 14517, PAD = 0, BOS = 1, EOS = 2, UNK = 3, OFF = 4; float chr_eps = 1e-5f;
    int U = 256, topk = 64, K = 256; float floor = -12.f, span_weight = 0.5f, read_prior = 0.25f, margin = 12.f, g_weight = 1.f; long long max_nodes = 4000000;
};

class V2Model {
public:
    explicit V2Model(const std::string& dir);
    const V2Config& config() const { return cfg_; }
    // syllable side: the final hidden states [L, d] fp32 for the key ids
    mc::array syl_hidden(const std::vector<int32_t>& key_ids) const;
    // The syllable model's character head: log P(character | all keys) at the given hidden rows, restricted to `cols`,
    // the characters the beam can actually use there (projecting the whole table is several times slower on long
    // rows).  Returns [rows, cols] fp32 on the host; empty when the export has no head.
    bool has_char_head() const { return has_char_head_; }
    int n_char() const { return n_char_; }
    std::vector<float> char_head_logits(const mc::array& hfull, const std::vector<int32_t>& rows, const std::vector<int32_t>& cols) const;
    // span log-softmax rows [n_spans, n_syl + 1] fp32 on the host, for spans (s[i], e[i]) of the hidden states
    std::vector<float> span_scores(const mc::array& h, const std::vector<int32_t>& s, const std::vector<int32_t>& e) const;

    // Character side with the tree K/V table.  A table is one context's: new_cache(root context tokens), then step(...) once
    // per beam position.  Every kept node stores the K/V of its own token once, and a hypothesis's context is the gather
    // of its ancestors' rows.  The table is paged: per layer NPAGES arrays of PAGE rows (k | v per row), so a step writes
    // only the pages it touches instead of copying the whole table per layer.  The attention kernel takes the pages as
    // separate buffers: Metal allows 31 buffers a kernel, and PAGE is baked into the kernel (shift 10, mask 1023).
    static constexpr int PAGE = 1024, NPAGES = 24;   // 24k slots; pages are allocated on first use
    struct KVCache {
        std::vector<std::vector<mc::array>> K;   // [layer][page] -> [PAGE, 2d + 4] int8: k | v per row in 8-bit, each half scaled by its absmax / 127, then the two fp16 scales
        int cap = 0, n_slots = 0, n_root = 0, n_pages = 0;   // pages allocated so far (the rest alias one shared zero page)
        std::vector<float> root_lp;         // log P(next | root context): the root hypothesis's own prediction
    };
    KVCache new_cache(const std::vector<int32_t>& root_tokens) const;
    // keep only the given old slots (in this order, after the root rows): the recomputation window needs the frozen beam's
    // parents and their ancestors, nothing else
    void compact(KVCache& c, const std::vector<int32_t>& keep_old_slots) const;
    struct Overflow : std::runtime_error { using std::runtime_error::runtime_error; };   // a decode needs more slots than the table has: the session resets and decodes cold
    // One new token per hypothesis: anc_slots = for each hypothesis the slots of its ancestors (oldest first), tok its
    // newest token, cols the vocabulary columns wanted.  Each computed hypothesis gets a slot (returned in `slots`;
    // reuse[i] >= 0 writes it into that existing slot instead), log P(col | prefix) for the requested columns in `lp`
    // ([n, cols] row-major), and its final hidden state (fp16, [n, d]) with the row's log-normaliser: from those any
    // column's log-probability is one dot product, so a prefix scored on an earlier keystroke never needs the model
    // again.  `part_*`: prefixes served from that store (hidden states [m, d] fp16, normalisers [m]); their columns come
    // back in part_lp ([m, cols]) from the same evaluation.
    struct StepOut { std::vector<float> lp, part_lp, lse; std::vector<uint16_t> hidden; };
    StepOut step(KVCache& c, const std::vector<std::vector<int32_t>>& anc_slots, int n_anc, const std::vector<int32_t>& tok, const std::vector<int32_t>& cols, std::vector<int32_t>& slots, const std::vector<int32_t>* reuse,
                 const std::vector<uint16_t>& part_hidden, const std::vector<float>& part_lse) const;
    mutable double prof_build_ms = 0, prof_eval_ms = 0; mutable long prof_steps = 0;   // step profile: graph construction (host) vs evaluation (GPU + sync)
private:
    const mc::array& W(const std::string& k) const;
    mc::array linear(const mc::array& x, const std::string& name) const;          // matmul + bias (addmm when fp16)
    mc::array ln(const mc::array& x, const std::string& name, float eps) const;    // fp32 out
    mc::array ln16(const mc::array& x, const std::string& name, float eps) const;  // fp16 in / out
    mc::array norm_emb(const std::vector<int32_t>& cols) const;
    V2Config cfg_; std::unordered_map<std::string, mc::array> w_; mc::Dtype dt_ = mc::float16;
    // The character model's normaliser is a sum over every column of the vocabulary, which is ordered by code point, so no
    // prefix of it carries the mass.  norm_rank.bin (written beside the export) lists the characters by corpus frequency;
    // the first NORM_TOP of them, unioned with the columns the beam is actually asking for, replace the full table in the
    // logsumexp.  The top 2048 cover nearly all character occurrences, and the shortlist makes each step faster and smaller.
    static constexpr int NORM_TOP = 2048;
    std::vector<int32_t> norm_ids_; mutable std::vector<char> norm_in_; mutable std::optional<mc::array> norm_emb_;   // norm_in_[c] = that column is already in the shortlist
    bool has_char_head_ = false; int n_char_ = 0;
    // Rows of the character model's position table (its training length).  A hypothesis's position is the root context
    // plus its characters, which an ordinary sentence keeps well inside; a long run of one-key syllables does not, and
    // positions past the table are read as its last row (see step()).
    int pos_rows_ = 0;
};
}  // namespace aq
#endif  // AQ_CPU
