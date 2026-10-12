// The v2 decoder: enumerate the key string into syllable arcs (with the tail rule) -> score the arcs with the syllable
// model (MLX) -> a joint beam over characters with the character model through a K/V table (MLX) -> candidates.
// decode_json speaks the JSON the host reads: {"candidates":[{text,score,fit_total,char_ends,reading,prefix}],...}.
#pragma once
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "enumerate.h"
#include "v2_model.h"

namespace aq {
struct V2CtxToken { int kind = 1; std::vector<int32_t> chars; std::vector<float> weights; };
struct V2Candidate { std::string text; float score = 0; std::vector<int32_t> char_ends, reading; std::string prefix; };
struct V2Timing { double lm_host_ms = 0, enumerate_ms = 0, syllable_ms = 0, spans_ms = 0, prune_ms = 0, lm_ms = 0, beam_ms = 0, finish_ms = 0, total_ms = 0; int n_arcs = 0, n_kept = 0, n_steps = 0, n_nodes = 0, n_lm_cached = 0, n_lm_computed = 0, n_slots_used = 0, n_resets = 0, n_frozen_positions = 0, n_compacted = 0, n_cache_reset = 0, n_front_from = 0;
    // The node cap: once the beam reaches max_nodes nothing more is pushed for the rest of the sentence.
    int overflowed = 0; };

class V2Session {
public:
    explicit V2Session(const std::string& dir);
    std::vector<V2Candidate> decode(const std::string& keys, const std::vector<V2CtxToken>& ctx, V2Timing* timing = nullptr);
    std::string decode_json(const std::string& keys, const std::string& ctx_json);
    const std::vector<std::string>& chars() const { return chars_; }
    // Incremental typing: the character model's rows and K/V entries are keyed by the text prefix they belong to, which
    // does not depend on the keys, so they stay valid from one keystroke to the next (and across backspaces).  A decode
    // after a keystroke re-walks the beam with fresh syllable scores and calls the model only for prefixes it has never
    // scored - the same candidates as a cold decode, for a fraction of the work.  reset() forgets everything (a commit,
    // or a new context).
    void reset();
    // The host calls this after the candidates of a keystroke have been shown (between keystrokes): it compacts the K/V
    // table to the rows the window can still use, so the work never lands inside a keystroke's latency.
    void maintain();
    // The syllables a clean key string spells, or empty when it does not segment exactly.  The personal memory stores a
    // phrase by its reading, and a commit that did not come from a decoder row has no reading to read off.
    std::vector<int32_t> exact_readings(const std::string& keys) const;
    // The speed tier, so the host can keep it across restarts: tier_fast() after a session, restore_tier() right after
    // opening the next one (instead of re-learning it over the first 16 keys).  restore_tier does nothing when the tier is
    // fixed (AQ_TIER=fast|accurate, AQ_BEAM_U, AQ_FINAL_U); auto mode goes on adjusting from there.
    bool tier_fast() const { return tier_fast_; }
    void restore_tier(bool fast) { if (tier_auto_) { set_tier(fast); key_ms_.clear(); } }
private:
    std::vector<V2Candidate> decode_once(const std::string& keys, const std::vector<V2CtxToken>& ctx, V2Timing* timing);   // may throw V2Model::Overflow
    bool compact_for_window(int q0, int extra = 0);
    std::vector<int32_t> encode_keys(const std::string& keys) const;

    std::unique_ptr<V2Model> model_; std::unique_ptr<Enumerator> en_;
    std::vector<std::string> keys_, syls_, chars_; std::unordered_map<std::string, int> kid_;
    std::vector<std::vector<int32_t>> syl_keyids_;           // per syllable its key ids (for the tail rule)
    std::unordered_map<std::string, std::vector<int32_t>> tail_index_;   // key-id prefix (1..6 ids, as bytes) -> syllables beginning with it (the tail rule in O(1) per length)
    std::vector<std::vector<int32_t>> fam_; std::vector<std::vector<float>> fam_lp_;
    int max_seg_len_ = 6; bool tail_rule_ = true; double fit_scale_ = 100.0;
    V2Config b_;                                              // beam settings (from the model config, with the overrides below)
    V2Model::KVCache kv_; bool kv_valid_ = false; std::string kv_ctx_;                      // persistent across decodes while the context is the same
    std::unordered_map<uint64_t, int32_t> slot_of_;                                          // text hash -> K/V slot
    std::vector<int32_t> colof_, cols_;                  // per-position scatter table and its column list, kept across decodes
    // The beam's "has this text already been pushed at this position" table: open addressing in one flat array, keyed by
    // the text hash mixed with the position, kept across keystrokes and emptied by bumping a generation counter.  A map
    // per position would allocate a node for each of the tens of thousands of hypotheses pushed per keystroke, which
    // costs time and fragments the heap so it cannot be returned.
    struct PosMap {
        std::vector<uint64_t> key; std::vector<int32_t> val; std::vector<uint32_t> tag;
        uint32_t gen = 0; size_t mask = 0, used = 0;
        void begin(size_t want) {
            size_t cap = 1024; while (cap < want * 2) cap <<= 1;
            if (cap > key.size()) { key.assign(cap, 0); val.assign(cap, 0); tag.assign(cap, 0); mask = cap - 1; gen = 0; }
            if (++gen == 0) { std::fill(tag.begin(), tag.end(), 0u); gen = 1; }     // wrapped: the old tags would read as live
            used = 0;
        }
        void grow() {
            std::vector<uint64_t> k2(key.size() * 2, 0); std::vector<int32_t> v2(key.size() * 2, 0); std::vector<uint32_t> t2(key.size() * 2, 0);
            const size_t m2 = k2.size() - 1;
            for (size_t i = 0; i < key.size(); i++) if (tag[i] == gen) { size_t j = key[i] & m2; while (t2[j] == gen) j = (j + 1) & m2; k2[j] = key[i]; v2[j] = val[i]; t2[j] = gen; }
            key.swap(k2); val.swap(v2); tag.swap(t2); mask = m2;
        }
        // the slot for (pos, h); *inserted is true when it was not there, and the caller writes the node id into it
        int32_t* find(int pos, uint64_t h, bool* inserted) {
            if (used * 2 >= key.size()) grow();
            const uint64_t k = h ^ (0x9E3779B97F4A7C15ull * (uint64_t)(pos + 1));
            size_t j = k & mask;
            while (tag[j] == gen) { if (key[j] == k) { *inserted = false; return &val[j]; } j = (j + 1) & mask; }
            tag[j] = gen; key[j] = k; val[j] = -1; used++; *inserted = true; return &val[j];
        }
    };
    PosMap hmap_;
    // The LM step's input buffers, kept across calls so a keystroke's steps do not allocate and free them again.
    std::vector<std::vector<int32_t>> anc_;              // sized by high-water mark; the live count is passed separately
    std::vector<int32_t> tok_, reuse_slots_;
    std::vector<int> which_, pwhich_;
    std::vector<uint16_t> ph_;
    std::vector<float> pl_;
    // Text hash -> final hidden state (fp16) and log-normaliser, so any column can be read on demand.  The states live in
    // one arena and the map stores offsets: one allocation per scored hypothesis left the heap holding far more than it
    // used and unable to give it back.
    struct Hidden { uint32_t off; float lse; };
    std::vector<uint16_t> hid_arena_, hid_arena_alt_;   // ping-pong: compaction fills alt and swaps, so neither side ever allocates again
    std::unordered_map<uint64_t, Hidden> lm_hidden_;
    const uint16_t* hidden_of(const Hidden& h) const { return hid_arena_.data() + (size_t)h.off; }

    // ---- the decode's settings, each chosen for accuracy against latency.
    // The recomputation window: on a keystroke that extends the previous keys, positions more than freeze_dist_ keys from
    // the end keep the previous keystroke's beam (their arc scores barely move that far back), and the front end
    // (enumeration and syllable pass) looks at only syl_ctx_ keys of context before the re-walked region.
    int freeze_dist_ = 12, syl_ctx_ = 32;
    // The syllable model's character head, interpolated against the causal character model (0.4 / 0.6, static reading
    // prior off) whenever the export carries the head.
    float char_head_w_ = 0.4f;
    // Per-character bonus: the same keys parsed into more syllables accumulate more (negative) arc and character
    // log-probabilities, so without it the beam is biased towards coarse segmentations.
    float bonus_ = 1.0f;
    // Temperature on the span head's log-probabilities, set per sentence from how clean the keys look (the share of start
    // keys that have a zero-cost arc): flattening the arc scores helps while the keys carry signal and hurts when most of
    // them are wrong.  Clean sentences get syl_temp_clean_ (syl_temp_clean_long_ above long_len_ keys, where latency is
    // tighter), fully noisy ones syl_temp_noisy_, and the rest in between.
    float syl_temp_clean_ = 1.6f, syl_temp_clean_long_ = 1.45f, syl_temp_noisy_ = 1.0f; int long_len_ = 24;
    // An arc that rewrote keys was read by the model from keys the writer never pressed, so its distribution is flattened
    // further: the temperature grows by ed_temp_ per edited key (up to three).
    float ed_temp_ = 0.15f;
    // The pruning margin in nats, for every length.
    float margin_ = 16.f;
    // A position may hold at most pos_cap_ * U hypotheses.  Only the top U of a position are expanded and only the top U
    // survive the garbage collection, so anything past that rank can never reach an output, and without the cap a long
    // sentence piles up enough of them to hit max_nodes.
    int pos_cap_ = 4;
    // Candidates are read from the beam at the last position, which expands no children, so a wider pool there costs one
    // step's extra rows and nothing else.
    int final_u_ = 1024;
    // Speed tiers (CPU builds): accurate (U 64 / final 192) or fast (48 / 128).  With AQ_TIER unset (auto) the session
    // keeps the last keystrokes' decode times and, between sentences only, moves to fast when their median is over
    // tier_slow_ms_ and back to accurate when it is under tier_fast_ms_.  AQ_TIER=fast|accurate, or AQ_BEAM_U /
    // AQ_FINAL_U, fix the widths instead.
    bool tier_auto_ = false, tier_fast_ = false;
    double tier_slow_ms_ = 150, tier_fast_ms_ = 70;
    std::vector<float> key_ms_;                                // the latest decode times, at most 32
    void set_tier(bool fast);
    void note_key_ms(double ms);
    struct PrevBeam { std::vector<char> nodes; std::vector<std::vector<int>> at; std::string keys, ctx; int nk = 0; } prev_;
    // maintain() compacts the K/V table once it holds more than this many rows.  A decode adds 2-5k with the window;
    // compacting more often drops hidden states the next keystroke has to recompute, less often raises the footprint.
    size_t max_slots_ = 5120;
};
std::string v2_json_escape(const std::string& s);
}  // namespace aq
