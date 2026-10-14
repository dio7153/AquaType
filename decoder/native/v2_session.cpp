#include "v2_session.h"
#include <algorithm>
#include <future>
#include <chrono>
#include <cmath>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <cstdlib>
#if defined(__APPLE__)
#include <malloc/malloc.h>
#elif defined(_WIN32)
#include <malloc.h>
#endif

namespace aq {
// hand freed host blocks back to the kernel: they otherwise stay in the footprint
#if defined(__APPLE__)
static void host_release_free_pages() { malloc_zone_pressure_relief(nullptr, 0); }
#elif defined(_WIN32)
static void host_release_free_pages() { _heapmin(); }
#else
static void host_release_free_pages() {}
#endif
static const bool V2TIME = std::getenv("AQ_V2_TIME") != nullptr;   // one line of stage timings per keystroke, to stderr
#ifdef AQ_CPU
using namespace cpu;
#else
using namespace mlx::core;
#endif
using clk = std::chrono::steady_clock;
static double ms_since(clk::time_point t) { return std::chrono::duration<double, std::milli>(clk::now() - t).count(); }

// ---- minimal JSON helpers (the exports are written by our own scripts; flat, predictable)
static std::string slurp(const std::string& p) { std::ifstream f(p); if (!f) throw std::runtime_error("cannot read " + p); std::stringstream ss; ss << f.rdbuf(); return ss.str(); }
static std::vector<std::string> json_str_list(const std::string& js, const std::string& key) {
    std::vector<std::string> out; size_t p = js.find("\"" + key + "\""); if (p == std::string::npos) return out; p = js.find('[', p); size_t e = p;
    while (true) { size_t q = js.find('"', e + 1); if (q == std::string::npos) break; size_t r = q + 1; std::string s;
        while (r < js.size() && js[r] != '"') { if (js[r] == '\\' && r + 1 < js.size()) { char c = js[r + 1]; if (c == 'u' && r + 5 < js.size()) { unsigned cp = std::stoul(js.substr(r + 2, 4), nullptr, 16); if (cp < 0x80) s += (char)cp; else if (cp < 0x800) { s += (char)(0xC0 | (cp >> 6)); s += (char)(0x80 | (cp & 0x3F)); } else { s += (char)(0xE0 | (cp >> 12)); s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F)); } r += 6; continue; } s += c; r += 2; continue; } s += js[r]; r++; }
        out.push_back(s); e = r; size_t nx = e + 1; while (nx < js.size() && (js[nx] == ' ' || js[nx] == '\n' || js[nx] == ',')) nx++; if (nx < js.size() && js[nx] == ']') break; }
    return out;
}
static std::vector<double> json_num_list(const std::string& js, const std::string& key) {
    std::vector<double> out; size_t p = js.find("\"" + key + "\""); if (p == std::string::npos) return out; p = js.find('[', p); size_t e = js.find(']', p); std::stringstream ss(js.substr(p + 1, e - p - 1)); std::string tok;
    while (std::getline(ss, tok, ',')) out.push_back(std::atof(tok.c_str())); return out;
}
static double json_number(const std::string& js, const std::string& key, double dflt) { size_t p = js.find("\"" + key + "\""); if (p == std::string::npos) return dflt; p = js.find(':', p); return std::atof(js.c_str() + p + 1); }
static bool json_bool(const std::string& js, const std::string& key, bool dflt) { size_t p = js.find("\"" + key + "\""); if (p == std::string::npos) return dflt; p = js.find(':', p); return js.compare(p + 2, 4, "true") == 0; }
std::string v2_json_escape(const std::string& s) { std::string o; for (unsigned char c : s) { if (c == '"') o += "\\\""; else if (c == '\\') o += "\\\\"; else if (c < 0x20) { char b[8]; snprintf(b, 8, "\\u%04x", c); o += b; } else o += (char)c; } return o; }

V2Session::V2Session(const std::string& dir) {
    set_cache_limit((size_t)4 << 20);   // MLX's buffer cache: every MB of it is footprint, and 4 MB costs no latency
    model_ = std::make_unique<V2Model>(dir); b_ = model_->config();
#ifdef AQ_CPU
    model_->stop = &abort_;                    // the syllable pass checks it between layers (MLX runs it as one GPU graph)
#endif
    std::string tj = slurp(dir + "/tables.json");
    keys_ = json_str_list(tj, "keys"); syls_ = json_str_list(tj, "syllables"); chars_ = json_str_list(tj, "chars");
    for (size_t i = 0; i < keys_.size(); i++) kid_[keys_[i]] = (int)i + 1;
    auto t = load_safetensors(dir + "/tables.safetensors"); auto& m = t.first;
    array syl_ids = m.at("syl_ids"), syl_len = m.at("syl_len"), nb = m.at("neighbor"), fam = m.at("fam"), fam_lp = m.at("fam_lp"); eval(syl_ids, syl_len, nb, fam, fam_lp);
    int NS = syl_ids.shape(0), ML = syl_ids.shape(1), K = nb.shape(0);
    for (int i = 0; i < NS; i++) {                                            // the edit distance works in fixed-size tables
        const int len = syl_len.data<int32_t>()[i];
        if (len < 1 || len > MAX_LEN || len > ML) throw std::runtime_error("tables: a syllable of " + std::to_string(len) + " keys");
    }
    NeighborMatrix nbm; nbm.K = K; nbm.m.assign(nb.data<uint8_t>(), nb.data<uint8_t>() + (size_t)K * K);
    ChannelCosts cc; cc.K = K; cc.sub = json_num_list(tj, "sub_cost"); cc.ins = json_num_list(tj, "ins_cost"); cc.del = json_number(tj, "c_del", 1.386); cc.tr = json_number(tj, "c_tr", 1.897);
#ifdef AQ_CPU
    // Spellings that follow how a word is said rather than a slip of the finger: Taiwanese Mandarin often merges
    // ㄣ/ㄥ, ㄢ/ㄤ and the retroflex/dental pairs ㄓㄔㄕ/ㄗㄘㄙ, so 應該 is typed ㄧㄣ ㄍㄞ.  The keys are not neighbours on
    // the keyboard, so the channel never offered the other reading; each pair becomes a substitution of its own, cheaper
    // than a typical neighbour slip (median 2.5).  AQ_FUZZY: pairs of keys ("p/ 0; 5y th gn" by default), "0" for none.
    {
        std::string pairs = "p/ 0; 5y th gn"; if (const char* e = std::getenv("AQ_FUZZY")) pairs = e;
        double cost = 0.5; if (const char* e = std::getenv("AQ_FUZZY_COST")) cost = std::atof(e);
        auto id = [&](char c) { auto it = kid_.find(std::string(1, c)); return it == kid_.end() ? -1 : it->second; };
        for (size_t i = 0; pairs != "0" && i + 1 < pairs.size(); i += 3) {
            const int a = id(pairs[i]), b = id(pairs[i + 1]); if (a < 0 || b < 0 || a >= K || b >= K) continue;
            for (auto [x, y] : {std::pair<int, int>{a, b}, {b, a}}) { nbm.m[(size_t)x * K + y] = 1; if ((size_t)x * K + y < cc.sub.size()) cc.sub[(size_t)x * K + y] = std::min(cc.sub[(size_t)x * K + y], cost); }
        }
    }
#endif
    max_seg_len_ = (int)json_number(tj, "max_seg_len", 6);
    if (max_seg_len_ < 1 || max_seg_len_ > MAX_LEN) throw std::runtime_error("tables.json: max_seg_len is outside 1.." + std::to_string(MAX_LEN)); fit_scale_ = json_number(tj, "fit_scale", 100.0); tail_rule_ = json_bool(tj, "tail_prefix_rule", true);
    en_ = std::make_unique<Enumerator>(std::vector<int32_t>(syl_ids.data<int32_t>(), syl_ids.data<int32_t>() + (size_t)NS * ML), std::vector<int32_t>(syl_len.data<int32_t>(), syl_len.data<int32_t>() + NS), ML, nbm, cc, fit_scale_, (int)json_number(tj, "max_edits", 3), max_seg_len_, (int)json_number(tj, "max_edits_long", 3), (int)json_number(tj, "long_seg_min", 5));
    en_->set_max_cache(200000);
    syl_keyids_.resize(NS); const int32_t* sid = syl_ids.data<int32_t>(); const int32_t* sl = syl_len.data<int32_t>();
    for (int i = 0; i < NS; i++) syl_keyids_[i].assign(sid + (size_t)i * ML, sid + (size_t)i * ML + sl[i]);
    for (int i = 0; i < NS; i++) for (int L = 1; L <= std::min(max_seg_len_, (int)syl_keyids_[i].size()); L++) { std::string k; for (int j = 0; j < L; j++) k += (char)(syl_keyids_[i][j] + 1); tail_index_[k].push_back(i); }
    int FM = fam.shape(1); const int32_t* f = fam.data<int32_t>(); const float* lp = fam_lp.data<float>(); fam_.resize(NS); fam_lp_.resize(NS);
    for (int i = 0; i < NS; i++) for (int j = 0; j < FM; j++) { int c = f[(size_t)i * FM + j]; if (c < 0) break; fam_[i].push_back(c); fam_lp_[i].push_back(lp[(size_t)i * FM + j]); }
    // With the character head, the character side is the head and the character model interpolated, and the static
    // reading prior is off.  These and the margin override what the export's model.json carries.
    if (model_->has_char_head()) { b_.read_prior = 0.f; b_.g_weight = 1.f - char_head_w_; }
    b_.margin = margin_;
#ifdef AQ_CPU
    // The beam widths were chosen against the GPU's cost per row; on the CPU every row is a full forward pass, so the
    // widths can be narrowed for latency (AQ_BEAM_U: rows expanded per position, AQ_FINAL_U: the last position's pool).
    // Defaults 64 / 192: on the evaluation sets (117 everyday sentences clean, with 10% key errors and with merged
    // pronunciations; confusable words; short words) they rank as 96 / 256 does, first place equal or better, at about
    // two thirds of the per-key work.  48 / 128 is faster again but loses a few third places with key errors.
    // A slow machine trades those third places for speed on its own (set_tier, note_key_ms).
    const char* tier = std::getenv("AQ_TIER");
    tier_auto_ = !tier || !*tier || std::string(tier) == "auto";
    set_tier(tier && std::string(tier) == "fast");
    // back to accurate well below the switch point: fast costs ~0.8 of accurate, so the gap keeps it from flapping
    if (const char* e = std::getenv("AQ_TIER_SLOW_MS")) if (std::atof(e) > 0) { tier_slow_ms_ = std::atof(e); tier_fast_ms_ = tier_slow_ms_ * 0.45; }
    if (const char* e = std::getenv("AQ_BEAM_U")) if (std::atoi(e) > 0) { b_.U = std::atoi(e); tier_auto_ = false; }
    if (const char* e = std::getenv("AQ_FINAL_U")) if (std::atoi(e) > 0) { final_u_ = std::atoi(e); tier_auto_ = false; }
    // the syllable pass costs in proportion to the keys it sees (syl_ctx_ of them before the re-walked region)
    if (const char* e = std::getenv("AQ_SYL_CTX")) if (std::atoi(e) > 0) syl_ctx_ = std::atoi(e);
    if (const char* e = std::getenv("AQ_SHORT_KEYS")) short_keys_ = std::atoi(e);
    if (const char* e = std::getenv("AQ_SHORT_U")) short_u_ = std::atoi(e);
    if (const char* e = std::getenv("AQ_SHORT_FINAL_U")) short_final_u_ = std::atoi(e);
#endif
    clear_cache(); host_release_free_pages();
}
// A commit ends the sentence, so everything the decode built is dead here.  clear() would not give it back - an
// unordered_map keeps its bucket array, a vector keeps its buffer - so the containers are swapped against empty ones,
// which destroys the storage.  lm_hidden_ is the bulk of it.
void V2Session::reset() {
    prev_ = PrevBeam(); kv_ = V2Model::KVCache(); kv_valid_ = false;
    std::string().swap(kv_ctx_);
    std::unordered_map<uint64_t, int32_t>().swap(slot_of_);
    std::unordered_map<uint64_t, Hidden>().swap(lm_hidden_);
    std::vector<uint16_t>().swap(hid_arena_); std::vector<uint16_t>().swap(hid_arena_alt_);
    clear_cache(); host_release_free_pages();
    if (tier_auto_ && key_ms_.size() >= 16) {                 // between sentences: the only time the widths may change
        std::vector<float> s = key_ms_; std::nth_element(s.begin(), s.begin() + s.size() / 2, s.end()); const double med = s[s.size() / 2];
        if (!tier_fast_ && med > tier_slow_ms_) { set_tier(true); key_ms_.clear(); }
        else if (tier_fast_ && med < tier_fast_ms_) { set_tier(false); key_ms_.clear(); }
    }
}
void V2Session::set_tier(bool fast) { tier_fast_ = fast; b_.U = fast ? 48 : 64; final_u_ = fast ? 128 : 192; }
void V2Session::note_key_ms(double ms) { key_ms_.push_back((float)ms); if (key_ms_.size() > 32) key_ms_.erase(key_ms_.begin()); }
std::vector<int32_t> V2Session::exact_readings(const std::string& keys) const {
    const std::vector<int32_t> k = encode_keys(keys);
    const int n = (int)k.size();
    if (n == 0) return {};
    // shortest-path over exact syllable spellings: prev[i] is the syllable ending at i and where it started
    std::vector<int> from(n + 1, -2), syl(n + 1, -1);
    from[0] = -1;
    for (int i = 0; i < n; i++) {
        if (from[i] == -2) continue;
        for (int L = 1; L <= max_seg_len_ && i + L <= n; L++) {
            std::string key; key.reserve(L);
            for (int j = 0; j < L; j++) key += (char)(k[i + j] + 1);
            auto it = tail_index_.find(key); if (it == tail_index_.end()) continue;
            for (int cand : it->second) {
                if ((int)syl_keyids_[cand].size() != L) continue;               // the whole syllable, not a prefix of one
                if (from[i + L] == -2) { from[i + L] = i; syl[i + L] = cand; }
                break;
            }
        }
    }
    if (from[n] == -2) return {};                                                // does not segment exactly
    std::vector<int32_t> out;
    for (int i = n; i > 0; i = from[i]) out.push_back(syl[i]);
    std::reverse(out.begin(), out.end());
    return out;
}
std::vector<int32_t> V2Session::encode_keys(const std::string& keys) const {
    std::vector<int32_t> out; size_t i = 0;
    while (i < keys.size()) { unsigned char c = keys[i]; size_t n = (c < 0x80) ? 1 : (c < 0xE0) ? 2 : (c < 0xF0) ? 3 : 4; std::string ch = keys.substr(i, n); auto it = kid_.find(ch); out.push_back(it == kid_.end() ? 0 : it->second); i += n; }
    return out;
}

// ---------------------------------------------------------------- the beam
namespace {
struct Node { float score; uint64_t h; int prev; int ch; int syl; int pos; };   // h = FNV hash of the character sequence
const uint64_t FNV_BASIS = 1469598103934665603ULL, FNV_PRIME = 1099511628211ULL;
}

// The K/V table has a fixed number of slots.  A decode that needs more than the table has left throws; the session then drops
// the table (and the row cache, which refers to its slots) and decodes the window again from an empty table.
std::vector<V2Candidate> V2Session::decode(const std::string& keys_str, const std::vector<V2CtxToken>& ctx, V2Timing* tm) {
    try { return decode_once(keys_str, ctx, tm); }
    catch (const V2Model::Overflow&) { kv_valid_ = false; if (tm) tm->n_resets = 1; return decode_once(keys_str, ctx, tm); }
}
void V2Session::maintain() {
    // The decode's allocations leave the malloc zone holding far more than it uses, and those free pages only go back to
    // the kernel when asked.  Unconditionally, and before the compaction guard below: the release has to happen on every
    // keystroke, compaction does not.
    host_release_free_pages();
    if (freeze_dist_ <= 0 || prev_.nk <= 0 || !kv_valid_ || kv_.n_slots <= (int)max_slots_) return;
    int q0 = std::min(prev_.nk, prev_.nk + 1 - freeze_dist_); if (q0 <= 1) return;
    compact_for_window(q0, 6, 6);                              // parents up to six positions past the line too (the host may coalesce a few keystrokes), and six before it (backspace)
}
// Keep the K/V rows the recomputation window will need - the top-U parents at positions [q0 - max_seg_len - back, q0 + extra) of
// the previous beam and their ancestor chains - and drop everything else, so the table (and the hidden-state store) stay
// small on a long sentence instead of resetting, which would forbid the window.  Returns false when a needed slot is
// missing (then a reset follows).
bool V2Session::compact_for_window(int q0, int extra, int back) {
    const Node* pn = (const Node*)prev_.nodes.data(); const int P = kv_.n_root; const int U = b_.U;
    std::vector<int32_t> keep; std::unordered_map<uint64_t, int32_t> slot2; std::unordered_map<uint64_t, Hidden> hid2; keep.reserve(4096);
    for (int q = std::max(0, q0 - max_seg_len_ - back); q < std::min(prev_.nk, q0 + extra); q++) {
        std::vector<int> ids = prev_.at[q]; if ((int)ids.size() > U) { std::partial_sort(ids.begin(), ids.begin() + U, ids.end(), [&](int x, int y) { return pn[x].score > pn[y].score; }); ids.resize(U); }
        for (int id : ids) for (int x = id; x >= 0 && pn[x].ch >= 0; x = pn[x].prev) {
            uint64_t h = pn[x].h; if (slot2.count(h)) break;                                   // the rest of this chain is already kept
            auto it = slot_of_.find(h); if (it == slot_of_.end()) return false;                 // nothing has been touched yet: a plain reset follows
            slot2[h] = P + (int32_t)keep.size(); keep.push_back(it->second); } }
    // Only now, with every slot found, move the kept hidden states: a move on a path that could still fail would leave
    // empty entries behind.  This is also the arena's garbage collection: the kept rows are copied into the other arena
    // in one pass, and the two swap, so after the first few keystrokes neither allocates again.
    std::vector<uint16_t>& arena2 = hid_arena_alt_; arena2.clear();
    if (arena2.capacity() < slot2.size() * (size_t)b_.chr_d) arena2.reserve(slot2.size() * (size_t)b_.chr_d);
    for (auto& kv : slot2) { auto hh = lm_hidden_.find(kv.first); if (hh == lm_hidden_.end()) continue;
        const uint16_t* hp = hidden_of(hh->second); hid2[kv.first] = Hidden{(uint32_t)arena2.size(), hh->second.lse};
        arena2.insert(arena2.end(), hp, hp + b_.chr_d); }
    model_->compact(kv_, keep); slot_of_.swap(slot2); lm_hidden_.swap(hid2); hid_arena_.swap(arena2); return true;   // alt now holds the old arena's storage for next time
}
// Whether every top-U node the window re-expands (prev_ positions [q0 - max_seg_len, q0)) still has its K/V row and its
// ancestors theirs.  Always true when the keys grew (those positions were expanded after the last compaction kept
// them); after a backspace the line moves back, possibly past what maintain() kept.
bool V2Session::window_has_rows(int q0) const {
    const Node* pn = (const Node*)prev_.nodes.data(); const int U = b_.U;
    for (int q = std::max(0, q0 - max_seg_len_); q < std::min(prev_.nk, q0); q++) {
        std::vector<int> ids = prev_.at[q]; if ((int)ids.size() > U) { std::partial_sort(ids.begin(), ids.begin() + U, ids.end(), [&](int x, int y) { return pn[x].score > pn[y].score; }); ids.resize(U); }
        for (int id : ids) for (int x = id; x >= 0 && pn[x].ch >= 0; x = pn[x].prev) if (!slot_of_.count(pn[x].h)) return false; }
    return true;
}

std::vector<V2Candidate> V2Session::decode_once(const std::string& keys_str, const std::vector<V2CtxToken>& ctx, V2Timing* tm) {
    const double build0 = model_->prof_build_ms, eval0 = model_->prof_eval_ms;
    auto t_all = clk::now(); V2Timing T0; V2Timing& T = tm ? *tm : T0;
    std::vector<int32_t> keys = encode_keys(keys_str); const int nk = (int)keys.size();
    // a short input may get a wider beam (short_keys_ / short_u_ / short_final_u_): it is cheap there, and a short word
    // whose first character ranks below U on its own is otherwise gone
    V2Config bw = b_; int final_u = final_u_;
    if (short_keys_ > 0 && nk <= short_keys_) { if (short_u_ > bw.U) bw.U = short_u_; if (short_final_u_ > final_u) final_u = short_final_u_; }
    const V2Config& b = bw; const int OFF = b.OFF;
    // ---- context: the max-weight character of each token as language-model prefix; frozen tokens also form the visible prefix
    // At most the last kMaxContext tokens, the host's own limit, so the root always fits the position table.
    constexpr size_t kMaxContext = 64;
    std::vector<int32_t> root{b.BOS}; std::string prefix;
    for (auto& t : (ctx.size() > kMaxContext ? std::vector<V2CtxToken>(ctx.end() - kMaxContext, ctx.end()) : ctx)) { if (t.chars.empty()) continue; int best = 0; for (size_t j = 1; j < t.chars.size(); j++) if (j < t.weights.size() && t.weights[j] > t.weights[best]) best = (int)j;
        int c = t.chars[best]; root.push_back(c >= 0 ? c + OFF : b.UNK); if (t.kind == 1 && c >= 0 && c < (int)chars_.size()) prefix += chars_[c]; }
    if (nk == 0) return {};
    // ---- enumerate (+ tail rule: syllables whose key ids start with the segment ending at the last key, cost 0)
    // The syllable model depends on the keys only: the lattice is enumerated on a second thread while this one encodes the
    // syllable graph and launches it (MLX encodes kernels on the calling thread).
    std::string ctx_key; for (int32_t t : root) { ctx_key += std::to_string(t); ctx_key += ','; }
    // The recomputation window's frozen line for this keystroke (positions < q0 keep the previous beam), decided first: only
    // arcs that can reach the last positions need fresh scores, so the enumeration and the syllable pass see keys[c0:] only.
    // Keys that grew keep the beam below the line; so do keys that shrank (backspace, a syllable deleted): every position
    // below the line covers keys both strings share, and on a long composition the cold decode is a second or more.
    const bool grew = keys_str.size() > prev_.keys.size() && keys_str.compare(0, prev_.keys.size(), prev_.keys) == 0;
    const bool shrank = keys_str.size() < prev_.keys.size() && prev_.keys.compare(0, keys_str.size(), keys_str) == 0;
    const bool freeze_possible = freeze_dist_ > 0 && prev_.nk > 0 && (grew || shrank) && prev_.ctx == ctx_key && kv_valid_ && ctx_key == kv_ctx_ &&
                                 (grew || window_has_rows(std::min(nk - freeze_dist_, prev_.nk)));
    const int q0_want = freeze_possible ? std::min(nk - freeze_dist_, prev_.nk) : 0;
    const int c0 = (q0_want > 1 && syl_ctx_ > 0) ? std::max(0, q0_want - max_seg_len_ - syl_ctx_) : 0;   // first key the front end looks at
    const std::vector<int32_t> keys_sub(keys.begin() + c0, keys.end());
    auto t0 = clk::now(); std::future<LatticeOut> lat_f = std::async(std::launch::async, [&]() { return en_->enumerate(keys_sub); });
    array h = model_->syl_hidden(keys_sub); async_eval(h);
    T.syllable_ms = ms_since(t0); t0 = clk::now();
    LatticeOut lat = lat_f.get();
    if (abort_.load(std::memory_order_relaxed)) throw Aborted{};                      // the syllable pass may have stopped short
    struct Arc { int s, e, y; float cost; int ned; }; std::vector<Arc> arcs; arcs.reserve(lat.cand_syl.size() + 4096);
    std::unordered_map<long long, size_t> tail_seen;
    for (size_t g = 0; g < lat.seg_start.size(); g++) for (int32_t a = lat.seg_offset[g]; a < lat.seg_offset[g + 1]; a++) {
        int ned = 0; for (int d = 0; d < 4; d++) ned += (int)lat.cand_fit[(size_t)a * FIT_DIMS + d];          // how many keys this reading had to change
        arcs.push_back({lat.seg_start[g] + c0, lat.seg_end[g] + c0, lat.cand_syl[a], (float)lat.cand_fit[(size_t)a * FIT_DIMS + 4] / (float)fit_scale_, ned}); }
    if (tail_rule_) {
        // A tail arc says "the keys so far are a prefix of this syllable", at cost 0; it also makes an existing arc for
        // the same syllable free.  This is what lets a syllable be offered while its keys are still being typed.
        for (size_t i = 0; i < arcs.size(); i++) if (arcs[i].e == nk - 1) tail_seen[((long long)arcs[i].s << 20) | arcs[i].y] = i;
        for (int L = 1; L <= std::min(max_seg_len_, nk); L++) { int s = nk - L; std::string k; for (int j = 0; j < L; j++) k += (char)(keys[s + j] + 1);
            auto ti = tail_index_.find(k); if (ti == tail_index_.end()) continue;
            for (int y : ti->second) {
                auto it = tail_seen.find(((long long)s << 20) | y);
                if (it != tail_seen.end()) arcs[it->second].cost = std::min(arcs[it->second].cost, 0.f);
                else arcs.push_back({s, nk - 1, y, 0.f}); } }
    }
    T.enumerate_ms = ms_since(t0); T.n_arcs = (int)arcs.size();
    // ---- syllable model: distinct spans once
    t0 = clk::now();
    std::vector<int> span_id((size_t)nk * nk, -1); std::vector<int32_t> ss, se; std::vector<int> arc_span(arcs.size());   // dense (start, end) -> span index
    for (size_t i = 0; i < arcs.size(); i++) { int& sid_ = span_id[(size_t)arcs[i].s * nk + arcs[i].e]; if (sid_ < 0) { sid_ = (int)ss.size(); ss.push_back(arcs[i].s - c0); se.push_back(arcs[i].e - c0); } arc_span[i] = sid_; }
    const int NSY = b.n_syl + 1; const std::vector<float> lpu = model_->span_scores(h, ss, se);
    // How corrupted this sentence is, as the decoder can see it: the share of start keys that have a zero-cost arc.  The
    // temperature follows it (see the header).
    float frac = 1.f;
    { std::vector<char> any(nk, 0), clean(nk, 0);
      for (size_t i = 0; i < arcs.size(); i++) { const int s0 = arcs[i].s; if (s0 < 0 || s0 >= nk) continue; any[s0] = 1; if (arcs[i].cost < 0.01f) clean[s0] = 1; }
      int na = 0, nc = 0; for (int s0 = 0; s0 < nk; s0++) { na += any[s0]; nc += clean[s0]; }
      frac = na ? (float)nc / (float)na : 1.f; }
    const float hi = (nk <= long_len_) ? syl_temp_clean_ : syl_temp_clean_long_;
    const float temp = syl_temp_noisy_ + (hi - syl_temp_noisy_) * frac;
    std::vector<float> score(arcs.size()), score_floor(arcs.size());
    for (size_t i = 0; i < arcs.size(); i++) {
        const float read_lp = lpu[(size_t)arc_span[i] * NSY + arcs[i].y];
        const float t_i = (ed_temp_ > 0.f && arcs[i].ned > 0) ? temp * (1.f + ed_temp_ * (float)std::min(arcs[i].ned, 3)) : temp;
        score[i] = read_lp / t_i - b.span_weight * arcs[i].cost;
        // The floor decides which arcs exist at all, and it must not move with the adaptive temperature, which would
        // tighten the lattice precisely on the noisiest sentences.  So it is tested against the clean temperature.
        score_floor[i] = read_lp / syl_temp_clean_ - b.span_weight * arcs[i].cost;
    }
    T.spans_ms = ms_since(t0); t0 = clk::now();
    // ---- per start: top-k above the floor
    std::vector<std::vector<int>> by_start_all(nk); for (size_t i = 0; i < arcs.size(); i++) if (score_floor[i] > b.floor && arcs[i].e < nk) by_start_all[arcs[i].s].push_back((int)i);
    std::vector<std::vector<int>> by_start(nk + 1); int n_kept = 0;
    for (int s = 0; s < nk; s++) { auto& v = by_start_all[s]; if ((int)v.size() > b.topk) { std::partial_sort(v.begin(), v.begin() + b.topk, v.end(), [&](int x, int y) { return score[x] > score[y]; }); v.resize(b.topk); } by_start[s] = v; n_kept += (int)v.size(); }
    // ---- the syllable model's character head, per kept arc, one value per family character.  Renormalised inside the
    // family so it is an opinion about the character only (the span head already scores the reading, and the raw head
    // would count it twice).  One matmul over the distinct start keys of the kept arcs.
    std::vector<std::vector<float>> arc_cb;
    if (model_->has_char_head()) {
        std::vector<int32_t> rows; std::vector<int> row_of(nk, -1);
        // only the positions the beam will actually expand: with the recomputation window everything before the frozen
        // line keeps its previous children
        const int s_lo = (q0_want > 1) ? std::max(0, q0_want - max_seg_len_) : 0;
        for (int s = s_lo; s < nk; s++) if (!by_start[s].empty()) { row_of[s] = (int)rows.size(); rows.push_back(s - c0); }
        // the characters the kept arcs can produce at those rows, deduplicated: a few hundred instead of the whole table
        std::vector<int32_t> cols; std::vector<int> col_of(model_->n_char(), -1);
        for (int s = s_lo; s < nk; s++) for (int i : by_start[s]) for (int c : fam_[arcs[i].y]) if (c >= 0 && c < (int)col_of.size() && col_of[c] < 0) { col_of[c] = (int)cols.size(); cols.push_back(c); }
        std::vector<float> lg = model_->char_head_logits(h, rows, cols); const int NC = (int)cols.size();
        if (!lg.empty()) {
            arc_cb.assign(arcs.size(), {});
            for (int s = s_lo; s < nk; s++) { if (row_of[s] < 0) continue; const float* row = lg.data() + (size_t)row_of[s] * NC;
                for (int i : by_start[s]) { const auto& f = fam_[arcs[i].y]; if (f.empty()) continue;
                    std::vector<float> v(f.size()); float mx = -1e30f;
                    for (size_t k = 0; k < f.size(); k++) { const int cc = (f[k] >= 0 && f[k] < (int)col_of.size()) ? col_of[f[k]] : -1; v[k] = cc >= 0 ? row[cc] : -30.f; mx = std::max(mx, v[k]); }
                    float z = 0.f; for (float x : v) z += std::exp(x - mx); const float lz = mx + std::log(z);
                    for (size_t k = 0; k < f.size(); k++) v[k] = char_head_w_ * (v[k] - lz);
                    arc_cb[i] = std::move(v); } }
        }
    }
    T.prune_ms = ms_since(t0); T.n_kept = n_kept;
    if (abort_.load(std::memory_order_relaxed)) throw Aborted{};                      // before the table is touched
    // ---- beam
    // reserved past the high-water mark of a long sentence (60-78k nodes), so a long row does not reallocate and copy
    // several megabytes in the middle of its own decode
    std::vector<Node> nodes; nodes.reserve(1 << 17); nodes.push_back({0.f, FNV_BASIS, -1, -1, -1, -1});
    std::vector<std::vector<int>> at(nk + 2); std::vector<float> best(nk + 2, -1e30f); at[0].push_back(0); best[0] = 0.f;
    std::vector<float> cut(nk + 2, -1e30f); std::vector<int> recheck(nk + 2, pos_cap_ * b.U); std::vector<float> cut_tmp;
    bool cache_reset = false;
    bool compacted = false;
    // Inside a keystroke the table is compacted only when it is about to run out; the host normally compacts between
    // keystrokes through maintain(), at max_slots_, because a compaction here costs 20-30 ms of that keystroke.
    const int hard = freeze_dist_ > 0 ? std::max((int)max_slots_, (int)kv_.cap - 8192) : (int)max_slots_;
    if (kv_valid_ && ctx_key == kv_ctx_ && kv_.n_slots > hard && q0_want > 1) compacted = compact_for_window(q0_want, 0);   // keep the frozen parents' rows, drop the rest
    T.n_compacted = compacted ? 1 : 0;
    if (!kv_valid_ || ctx_key != kv_ctx_ || (kv_.n_slots > hard && !compacted)) { cache_reset = true;
        kv_ = V2Model::KVCache(); slot_of_.clear(); lm_hidden_.clear(); clear_cache();          // release the old table before the new one exists, so two are never alive at once
        hid_arena_.clear();                // the hidden states lm_hidden_ indexed: kept, they piled up with every context change
        kv_ = model_->new_cache(root); kv_valid_ = true; kv_ctx_ = ctx_key; T.n_cache_reset = 1; }
    const int slots_at_start = kv_.n_slots;
    V2Model::KVCache& cache = kv_;
    std::vector<int> cur; std::vector<float> eos_lp; bool over = false, reached_end = false; double lm_ms = 0, beam_ms = 0; int n_cached = 0, n_computed = 0;
    hmap_.begin((size_t)b.U * (size_t)(nk + 2));                  // per position: text hash -> node, so the beam holds distinct texts
    int q0 = 0;                                                        // positions < q0 are frozen: copied from the previous keystroke's beam
    if (freeze_possible && !cache_reset) {
        q0 = q0_want;
        if (q0 > 1) {
            nodes.assign((const Node*)prev_.nodes.data(), (const Node*)prev_.nodes.data() + prev_.nodes.size() / sizeof(Node));
            for (int q = 0; q < q0; q++) { at[q] = prev_.at[q]; best[q] = -1e30f; for (int id : at[q]) best[q] = std::max(best[q], nodes[id].score); }
        } else q0 = 0;
    }
    T.n_frozen_positions = q0; T.n_front_from = c0;
    // The next keystroke inherits this beam: positions <= n_done (all of them for a finished decode).  Only the top-U nodes
    // of each position and their ancestor chains can ever be used again (the beam expands the top U of a position;
    // candidates and compaction follow chains), so the list is garbage-collected and renumbered here; otherwise every
    // keystroke's dead expansions would be carried along until the node list hit max_nodes.
    auto keep_beam = [&](int n_done) {
        std::vector<char> mark(nodes.size(), 0); mark[0] = 1;
        const int keepU = b.U;
        for (size_t q = 0; q < at.size(); q++) { auto& lst = at[q];
            if ((int)q > n_done) { lst.clear(); continue; }                                       // half-built: not handed on
            if ((int)lst.size() > keepU) { std::partial_sort(lst.begin(), lst.begin() + keepU, lst.end(), [&](int x, int y) { return nodes[x].score > nodes[y].score; }); lst.resize(keepU); }
            for (int id : lst) for (int x = id; x >= 0 && !mark[x]; x = nodes[x].prev) mark[x] = 1;
        }
        std::vector<int> newid(nodes.size(), -1); std::vector<Node> kept; kept.reserve(nodes.size() / 4 + 1024);
        for (size_t i = 0; i < nodes.size(); i++) if (mark[i]) { newid[i] = (int)kept.size(); kept.push_back(nodes[i]); }
        // a better derivation found later can point a node at a parent with a higher index, so parents are remapped in a second pass
        for (auto& n : kept) if (n.prev >= 0) n.prev = newid[n.prev];
        for (auto& lst : at) for (int& id : lst) id = newid[id];
        prev_.nodes.assign((const char*)kept.data(), (const char*)kept.data() + kept.size() * sizeof(Node)); prev_.at = std::move(at); prev_.keys = keys_str; prev_.ctx = ctx_key; prev_.nk = n_done;
    };
    for (int q = 0; q <= nk; q++) {
        if (q < q0 - max_seg_len_) continue;                                                       // frozen, and no arc from here reaches the fresh region
        if (abort_.load(std::memory_order_relaxed)) {                                              // between steps
            // Every arc into q starts before q, so positions <= q are complete, and every node this decode expanded has its
            // K/V row.  Past this decode's own frozen line they are handed on like a finished beam: on a machine slower
            // than the typing every decode is stopped, and without this the next one would start again from the last
            // decode that finished, its window growing with every stopped keystroke.  Otherwise prev_ stays, and a table
            // compacted for this window does not fit it.
            if (freeze_dist_ > 0 && q > q0 && q > 1) keep_beam(q); else if (compacted) kv_valid_ = false;
            throw Aborted{}; }
        auto tq = clk::now(); auto& src = at[q]; if (src.empty()) continue;
        // only the top U of a position are ever expanded, so the rest does not need ordering: O(n log U), not O(n log n)
        if (src.size() <= (size_t)b.U) std::sort(src.begin(), src.end(), [&](int x, int y) { return nodes[x].score > nodes[y].score; });
        else std::partial_sort(src.begin(), src.begin() + b.U, src.end(), [&](int x, int y) { return nodes[x].score > nodes[y].score; });
        {
            // The last position expands no children - it only scores end-of-sentence and hands the candidates out - so the
            // pool there costs one step's extra rows, and a step's cost is its kernel launches, not its rows.
            const size_t take = (q == nk && final_u > b.U) ? (size_t)final_u : (size_t)b.U;
            cur.assign(src.begin(), src.begin() + std::min(take, src.size()));                   // distinct texts already (dedup at push), so top U
        }
        if (q < nk && by_start[q].empty()) { beam_ms += ms_since(tq); continue; }                     // nothing leaves this position: the model call would be wasted
        // The columns this position reads: EOS and every family character of the arcs leaving q (the root row is
        // full-vocabulary).  colof is a vocabulary-sized scatter table kept across positions and cleared through the
        // previous position's column list, which is exactly the set of entries that were written.
        if ((int)colof_.size() != b.vocab) colof_.assign(b.vocab, -1);
        std::vector<int32_t>& colof = colof_;
        for (int32_t c : cols_) colof[c] = -1;
        cols_.clear(); cols_.push_back(b.EOS); colof[b.EOS] = 0;
        std::vector<int32_t>& cols = cols_;
        if (q < nk) for (int i : by_start[q]) for (int ch : fam_[arcs[i].y]) if (colof[ch + OFF] < 0) { colof[ch + OFF] = (int)cols.size(); cols.push_back(ch + OFF); }
        const int M = (int)cols.size();
        // Language model for the kept hypotheses: root -> stored prediction; a prefix scored on an earlier keystroke -> its
        // cached hidden state; the rest -> one new token each through the K/V table (their ancestors' slots exist: an
        // ancestor was kept, so scored).
        std::vector<int> rowof(cur.size(), -1), partof(cur.size(), -1);
        V2Model::StepOut so;
        auto run_lm = [&]() {
            auto& anc = anc_; size_t n_anc = 0;                      // anc_ keeps its rows' capacity; n_anc is the live count
            auto& tok = tok_; auto& reuse_slots = reuse_slots_; auto& which = which_; auto& pwhich = pwhich_; auto& ph = ph_; auto& pl = pl_;
            tok.clear(); reuse_slots.clear(); which.clear(); pwhich.clear(); ph.clear(); pl.clear();
            for (size_t u = 0; u < cur.size(); u++) { int id = cur[u]; if (nodes[id].ch < 0) continue;
                auto it = lm_hidden_.find(nodes[id].h);                                          // scored before (any keystroke, any columns): served from its hidden state
                if (it != lm_hidden_.end()) { partof[u] = (int)pwhich.size(); pwhich.push_back((int)u); const uint16_t* hp = hidden_of(it->second); ph.insert(ph.end(), hp, hp + b_.chr_d); pl.push_back(it->second.lse); n_cached++; continue; }
                if (n_anc >= anc.size()) anc.emplace_back();
                std::vector<int32_t>& a = anc[n_anc]; a.clear();
                bool have = true; auto own = slot_of_.find(nodes[id].h); reuse_slots.push_back(own == slot_of_.end() ? -1 : own->second);
                for (int p = nodes[id].prev; p >= 0 && nodes[p].ch >= 0; p = nodes[p].prev) { auto sl = slot_of_.find(nodes[p].h); if (sl == slot_of_.end()) { have = false; break; } a.push_back(sl->second); }
                if (!have) throw std::runtime_error("v2 session: an ancestor prefix has no K/V slot");
                std::reverse(a.begin(), a.end()); n_anc++; tok.push_back(nodes[id].ch + OFF); which.push_back((int)u); n_computed++; }
            beam_ms += ms_since(tq); auto tl = clk::now();
            if (n_anc || !pwhich.empty()) { std::vector<int32_t> slots; so = model_->step(cache, anc, (int)n_anc, tok, cols, slots, &reuse_slots, ph, pl);
                auto th = clk::now(); const size_t D = which.empty() ? 0 : so.hidden.size() / which.size();
                for (size_t j = 0; j < which.size(); j++) { int id = cur[which[j]]; rowof[which[j]] = (int)j; slot_of_[nodes[id].h] = slots[j];
                    auto ins = lm_hidden_.try_emplace(nodes[id].h, Hidden{(uint32_t)hid_arena_.size(), 0.f});
                    if (ins.second) hid_arena_.insert(hid_arena_.end(), so.hidden.begin() + j * D, so.hidden.begin() + (j + 1) * D);
                    else std::copy(so.hidden.begin() + j * D, so.hidden.begin() + (j + 1) * D, hid_arena_.begin() + ins.first->second.off);
                    ins.first->second.lse = so.lse[j]; }
                T.lm_host_ms += ms_since(th); if (n_anc) T.n_steps++; }
            lm_ms += ms_since(tl); tq = clk::now();
        };
        auto lpof = [&](size_t u, int token) -> float { if (partof[u] >= 0) return so.part_lp[(size_t)partof[u] * M + colof[token]]; int j = rowof[u]; return j < 0 ? cache.root_lp[token] : so.lp[(size_t)j * M + colof[token]]; };
        auto push_children = [&]() {
            for (int i : by_start[q]) { const Arc& A = arcs[i]; int e = A.e, y = A.y; if (e + 1 < q0) continue; const auto& f = fam_[y]; const auto& flp = fam_lp_[y];
                const float* cbi = (!arc_cb.empty() && !arc_cb[i].empty()) ? arc_cb[i].data() : nullptr;
                for (size_t u = 0; u < cur.size() && !over; u++) { const Node hN = nodes[cur[u]];
                    for (size_t k = 0; k < f.size(); k++) {
                        const int c = f[k]; const float t_lm = b.g_weight * lpof(u, c + OFF), t_head = (cbi ? cbi[k] : 0.f), t_prior = b.read_prior * flp[k];
                        float sc = hN.score + score[i] + t_lm + t_prior + t_head + bonus_;
                        if (sc < best[e + 1] - b.margin || sc < cut[e + 1]) continue; if (sc > best[e + 1]) best[e + 1] = sc;
                        uint64_t hh = (hN.h ^ (uint64_t)(c + 1)) * FNV_PRIME;
                        bool fresh = false; int32_t* hslot = hmap_.find(e + 1, hh, &fresh);
                        if (!fresh) { Node& ex = nodes[*hslot]; if (ex.score < sc) ex = {sc, hh, cur[u], c, y, q}; continue; }   // same text: keep the better-scoring derivation
                        if ((long long)nodes.size() >= b.max_nodes) { over = true; break; }
                        *hslot = (int32_t)nodes.size(); at[e + 1].push_back((int)nodes.size()); nodes.push_back({sc, hh, cur[u], c, y, q});
                        // Each position gets an admission floor at its pos_cap-th best score, refreshed whenever it has grown by
                        // half again (see pos_cap_ in the header).
                        if ((int)at[e + 1].size() >= recheck[e + 1]) {
                            const int cap = pos_cap_ * b.U; std::vector<float>& tv = cut_tmp; tv.resize(at[e + 1].size());
                            for (size_t j = 0; j < at[e + 1].size(); j++) tv[j] = nodes[at[e + 1][j]].score;
                            std::nth_element(tv.begin(), tv.begin() + (cap - 1), tv.end(), std::greater<float>()); cut[e + 1] = tv[cap - 1];
                            recheck[e + 1] = (int)at[e + 1].size() + cap / 2; } } } }
        };
        if (q == nk) { run_lm(); eos_lp.resize(cur.size()); for (size_t u = 0; u < cur.size(); u++) eos_lp[u] = lpof(u, b.EOS); beam_ms += ms_since(tq); reached_end = true; break; }
        run_lm(); push_children();
        beam_ms += ms_since(tq);
    }
    T.overflowed = over ? 1 : 0;
    // No hypothesis reached the last key (every arc into it below the floor, or the node cap hit): what the beam last
    // held covers only the keys before it, and handed out it would claim to cover them all.  Nothing is the answer.
    if (!reached_end) cur.clear();
    T.lm_ms = lm_ms; T.beam_ms = beam_ms; T.n_nodes = (int)nodes.size(); T.n_lm_cached = n_cached; T.n_lm_computed = n_computed; t0 = clk::now();
    // ---- finish: top K distinct texts
    std::vector<std::pair<float, int>> fs; fs.reserve(cur.size());
    for (size_t u = 0; u < cur.size(); u++) fs.push_back({nodes[cur[u]].score + b.g_weight * (u < eos_lp.size() ? eos_lp[u] : 0.f), cur[u]});
    std::sort(fs.begin(), fs.end(), [](auto& a, auto& b2) { return a.first > b2.first; });
    std::vector<V2Candidate> out; std::unordered_map<uint64_t, int> seen;
    for (auto& p : fs) { if (!seen.emplace(nodes[p.second].h, p.second).second) continue;
        V2Candidate c; std::vector<int> chs, syls, poss; for (int id = p.second; id >= 0 && nodes[id].ch >= 0; id = nodes[id].prev) { chs.push_back(nodes[id].ch); syls.push_back(nodes[id].syl); poss.push_back(nodes[id].pos); }
        std::reverse(chs.begin(), chs.end()); std::reverse(syls.begin(), syls.end()); std::reverse(poss.begin(), poss.end());
        for (size_t j = 0; j < chs.size(); j++) { c.text += chars_[chs[j]]; c.reading.push_back(syls[j]); c.char_ends.push_back(j + 1 < poss.size() ? poss[j + 1] - 1 : nk - 1); }
        c.score = p.first; c.prefix = prefix; out.push_back(std::move(c)); if ((int)out.size() >= b.K) break; }
    if (freeze_dist_ > 0) keep_beam(nk);
    T.n_slots_used = cache.n_slots - slots_at_start; T.finish_ms = ms_since(t0); T.total_ms = ms_since(t_all);
    // build vs eval: the step's cost splits into host-side MLX graph construction and the GPU evaluation it waits on
    if (V2TIME) std::fprintf(stderr, "[v2time] nk %3d enum %5.1f syl %5.1f spans %5.1f prune %5.1f lm %6.1f (build %5.1f eval %5.1f host %5.1f) beam %6.1f finish %5.1f | total %6.1f ms | arcs %d nodes %d steps %d lm hit %d miss %d slots %d\n",
                             nk, T.enumerate_ms, T.syllable_ms, T.spans_ms, T.prune_ms, T.lm_ms,
                             model_->prof_build_ms - build0, model_->prof_eval_ms - eval0, T.lm_host_ms, T.beam_ms, T.finish_ms, T.total_ms,
                             T.n_arcs, T.n_nodes, T.n_steps, T.n_lm_cached, T.n_lm_computed, T.n_slots_used);
    return out;
}

std::string V2Session::decode_json(const std::string& keys, const std::string& ctx_json) {
    std::vector<V2CtxToken> ctx;
    size_t p = 0;   // [{"kind":k,"chars":[...],"weights":[...]}, ...]
    while ((p = ctx_json.find("\"kind\"", p)) != std::string::npos) { V2CtxToken t; t.kind = (int)std::atof(ctx_json.c_str() + ctx_json.find(':', p) + 1);
        size_t c = ctx_json.find("\"chars\"", p), w = ctx_json.find("\"weights\"", p), end = ctx_json.find('}', p);
        auto nums = [&](size_t at) { std::vector<double> v; size_t a = ctx_json.find('[', at), e = ctx_json.find(']', a); std::stringstream ss(ctx_json.substr(a + 1, e - a - 1)); std::string tok; while (std::getline(ss, tok, ',')) if (!tok.empty()) v.push_back(std::atof(tok.c_str())); return v; };
        if (c != std::string::npos && c < end) for (double x : nums(c)) t.chars.push_back((int32_t)x);
        if (w != std::string::npos && w < end) for (double x : nums(w)) t.weights.push_back((float)x);
        if (end == std::string::npos) break;                                // a token cut short: the rest is not context
        ctx.push_back(t); p = end + 1; }
    abort_.store(false, std::memory_order_relaxed);
    V2Timing T; std::vector<V2Candidate> cands;
    try { cands = decode(keys, ctx, &T); } catch (const Aborted&) { return "{\"aborted\":true,\"candidates\":[]}"; }
    note_key_ms(T.total_ms);
    std::ostringstream o; o << "{\"candidates\":[";
    for (size_t i = 0; i < cands.size(); i++) { const auto& c = cands[i]; if (i) o << ","; o << "{\"text\":\"" << v2_json_escape(c.text) << "\",\"score\":" << c.score << ",\"fit_total\":0,\"char_ends\":[";
        for (size_t j = 0; j < c.char_ends.size(); j++) { if (j) o << ","; o << c.char_ends[j]; } o << "],\"reading\":["; for (size_t j = 0; j < c.reading.size(); j++) { if (j) o << ","; o << c.reading[j]; } o << "],\"prefix\":\"" << v2_json_escape(c.prefix) << "\"}"; }
    o << "],\"prof\":{\"lm_build_ms\":" << model_->prof_build_ms << ",\"lm_eval_ms\":" << model_->prof_eval_ms << ",\"lm_steps\":" << model_->prof_steps << ",\"lm_host_ms\":" << T.lm_host_ms << ",\"tier\":\"" << (tier_fast_ ? "fast" : "accurate") << "\"},\"timing_ms\":{\"enumerate\":" << T.enumerate_ms << ",\"syllable\":" << T.syllable_ms << ",\"spans\":" << T.spans_ms << ",\"prune\":" << T.prune_ms << ",\"lm\":" << T.lm_ms << ",\"beam\":" << T.beam_ms << ",\"finish\":" << T.finish_ms << ",\"total\":" << T.total_ms << "},\"counts\":{\"arcs\":" << T.n_arcs << ",\"kept\":" << T.n_kept << ",\"steps\":" << T.n_steps << ",\"nodes\":" << T.n_nodes << ",\"lm_cached\":" << T.n_lm_cached << ",\"lm_computed\":" << T.n_lm_computed << ",\"slots_used\":" << T.n_slots_used << ",\"resets\":" << T.n_resets << ",\"frozen_positions\":" << T.n_frozen_positions << ",\"compacted\":" << T.n_compacted << ",\"cache_reset\":" << T.n_cache_reset << ",\"overflowed\":" << T.overflowed << "}}";
    return o.str();
}
}  // namespace aq
