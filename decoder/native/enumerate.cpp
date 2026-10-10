// Candidate enumeration for a key window: segments of length 1..max_seg_len ending at every key, candidates =
// syllables within max_edits (via edit_all), cached per segment key-string.  Each candidate carries FIT_DIMS = 4 op
// counts + the channel cost of its cheapest alignment inside the edit budget (channel_one), stored as int16 fixed point
// (round-half-up of cost x fit_scale).
#include "enumerate.h"

namespace aq {

Enumerator::Enumerator(std::vector<int32_t> syl_ids, std::vector<int32_t> syl_len, int max_len, NeighborMatrix nb, ChannelCosts cc, double fit_scale, int max_edits, int max_seg_len, int max_edits_long, int long_seg_min)
    : syl_ids_(std::move(syl_ids)), syl_len_(std::move(syl_len)), max_len_(max_len), nb_(std::move(nb)), cc_(std::move(cc)), fit_scale_(fit_scale), max_edits_(max_edits), max_seg_len_(max_seg_len), max_edits_long_(max_edits_long), long_seg_min_(long_seg_min) {
    edits_buf_.resize(syl_len_.size()); ops_buf_.resize(syl_len_.size() * 4);
}

const SegCands& Enumerator::segment(const std::vector<int32_t>& keys, int s, int e) {
    std::string k; k.reserve((e - s + 1) * 2);
    for (int i = s; i <= e; i++) { k.push_back((char)(keys[i] & 0xff)); k.push_back((char)((keys[i] >> 8) & 0xff)); }
    auto it = cache_.find(k);
    if (it != cache_.end()) return it->second;
    SyllableTable tab; tab.count = (int)syl_len_.size(); tab.max_len = max_len_; tab.ids = syl_ids_.data(); tab.len = syl_len_.data();
    int me = (e - s + 1 >= long_seg_min_) ? max_edits_long_ : max_edits_;      // menu rule by segment length
    int n = e - s + 1; const int* obs = keys.data() + s;
    edit_all(obs, n, tab, nb_, me, edits_buf_.data(), ops_buf_.data());
    SegCands sc;
    for (int i = 0; i < tab.count; i++) if (edits_buf_[i] <= me) {
        sc.syl.push_back(i); for (int j = 0; j < FIT_COUNT_DIMS; j++) sc.fit.push_back(ops_buf_[4 * i + j]);
        double cost = channel_one(obs, n, tab.ids + i * tab.max_len, tab.len[i], nb_, cc_, me);      // survivors only (a few % of the table)
        sc.fit.push_back(channel_fixed(cost, fit_scale_));
    }
    cache_bytes_ += k.size() + sc.syl.size() * sizeof(int32_t) + sc.fit.size() * sizeof(int16_t) + 64;
    return cache_.emplace(std::move(k), std::move(sc)).first->second;
}

LatticeOut Enumerator::enumerate(const std::vector<int32_t>& keys) {
    if (cache_.size() >= max_cache_ || cache_bytes_ >= max_cache_bytes_) { cache_.clear(); cache_bytes_ = 0; }      // between windows only: references handed out inside one window stay valid
    LatticeOut out; out.seg_offset.push_back(0); int T = (int)keys.size();
    for (int e = 0; e < T; e++) {
        for (int L = 1; L <= max_seg_len_; L++) {
            int s = e - L + 1; if (s < 0) break;
            const SegCands& sc = segment(keys, s, e);
            if (sc.syl.empty()) continue;
            int sidx = (int)out.seg_start.size(); out.seg_start.push_back(s); out.seg_end.push_back(e);
            for (size_t i = 0; i < sc.syl.size(); i++) {
                out.cand_seg.push_back(sidx); out.cand_syl.push_back(sc.syl[i]);
                for (int j = 0; j < FIT_DIMS; j++) out.cand_fit.push_back(sc.fit[FIT_DIMS * i + j]);
            }
            out.seg_offset.push_back((int32_t)out.cand_syl.size());
        }
    }
    return out;
}

}  // namespace aq
