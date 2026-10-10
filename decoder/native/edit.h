#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace aq {

constexpr int MAX_LEN = 8;   // max keys per segment or syllable handled by the DP
constexpr int FIT_DIMS = 5;        // fit vector per candidate: 4 op counts + channel cost (fixed point)
constexpr int FIT_COUNT_DIMS = 4;  // the op-count dims (zero-edit test and lambda x edits use these only)
constexpr double BIGC = 1e9;       // unreachable channel cost

struct Ops { int8_t sub, ins, del, tr; };
struct EditResult { int edits; Ops ops; };

struct NeighborMatrix {
    int K = 0;                         // number of key ids incl. 0 (unknown)
    std::vector<uint8_t> m;            // K*K
    bool is_neighbor(int a, int b) const { return m[(size_t)a * K + b] != 0; }
};

// Channel costs (-log probabilities from the noise model's op prior and keyboard table; built by
// decoder/core/ref_edit.py KeyTables): sub[a*K+b] = cost of typing key a when meaning key b (BIGC unless neighbours);
// ins[x*K+k] = cost of an extra key x typed before the intended key k (random / neighbour / repeat shares).
struct ChannelCosts {
    int K = 0; std::vector<double> sub, ins; double del = 0, tr = 0;
    double sub_cost(int a, int b) const { return sub[(size_t)a * K + b]; }
    double ins_cost(int x, int k) const { return ins[(size_t)x * K + k]; }
};

struct SyllableTable {
    int count = 0, max_len = 0;
    const int* ids = nullptr;          // count * max_len key ids (0-padded)
    const int* len = nullptr;          // count
};

EditResult edit_one(const int* obs, int n, const int* cand, int m, const NeighborMatrix& nb, int max_edits);
void edit_all(const int* obs, int n, const SyllableTable& tab, const NeighborMatrix& nb, int max_edits, int* edits_out, int8_t* ops_out);
// Weighted edit distance: cheapest alignment of obs with cand using at most max_edits edits under the same
// move set as edit_one; BIGC when no alignment fits the budget. Semantics defined by ref_edit.channel_one.
double channel_one(const int* obs, int n, const int* cand, int m, const NeighborMatrix& nb, const ChannelCosts& cc, int max_edits);
void channel_all(const int* obs, int n, const SyllableTable& tab, const NeighborMatrix& nb, const ChannelCosts& cc, int max_edits, double* cost_out);
int16_t channel_fixed(double cost, double scale);    // round-half-up fixed point shared with the reference

}  // namespace aq
