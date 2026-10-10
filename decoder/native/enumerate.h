#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include "edit.h"

namespace aq {

struct SegCands {                       // cached candidates of one segment key-string
    std::vector<int32_t> syl;           // syllable ids
    std::vector<int16_t> fit;           // FIT_DIMS per candidate: 4 op counts + channel cost (fixed point)
};

struct LatticeOut {
    std::vector<int32_t> seg_start, seg_end, seg_offset, cand_seg, cand_syl;
    std::vector<int16_t> cand_fit;      // FIT_DIMS per candidate
};

class Enumerator {
public:
    Enumerator(std::vector<int32_t> syl_ids, std::vector<int32_t> syl_len, int max_len, NeighborMatrix nb, ChannelCosts cc, double fit_scale, int max_edits, int max_seg_len, int max_edits_long = 3, int long_seg_min = 99);   // segments of >= long_seg_min keys tolerate max_edits_long
    // keys: key ids (1..K, 0 unknown), length T <= window. Uses the segment cache; calling with a longer string
    // (one more key) only computes the <= max_seg_len new segments ending at the new key.
    LatticeOut enumerate(const std::vector<int32_t>& keys);
    const SegCands& segment(const std::vector<int32_t>& keys, int s, int e);   // cached lookup / compute
    size_t cache_size() const { return cache_.size(); }
    void clear_cache() { cache_.clear(); cache_bytes_ = 0; }
    void set_max_cache(size_t n) { max_cache_ = n; }      // memory bound: the cache is reset (not evicted) when it reaches n entries, checked between windows
private:
    std::vector<int32_t> syl_ids_, syl_len_; int max_len_; NeighborMatrix nb_; ChannelCosts cc_; double fit_scale_; int max_edits_, max_seg_len_, max_edits_long_, long_seg_min_;
    std::unordered_map<std::string, SegCands> cache_;
    size_t max_cache_ = 1000000;
    size_t max_cache_bytes_ = 8u << 20, cache_bytes_ = 0;   // the same reset at 8 MB of cached candidates: a 36-key window adds ~0.4 MB, and a long typing session must not grow the footprint
    std::vector<int32_t> edits_buf_; std::vector<int8_t> ops_buf_;
};

}  // namespace aq
