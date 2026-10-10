// Restricted Damerau edit distance with neighbour-only substitution, cutoff, canonical op vectors.
// Semantics and tie-break order are defined by decoder/core/ref_edit.py and must match it exactly.
#include "edit.h"
#include <algorithm>
#include <cstring>
#include <cmath>

namespace aq {

static const int BIG = 99;

EditResult edit_one(const int* obs, int n, const int* cand, int m, const NeighborMatrix& nb, int max_edits) {
    // D[i][j] cost, O[i][j] op vector (sub, ins, del, tr); n, m <= MAX_LEN, which the session checks when it loads
    int D[MAX_LEN + 1][MAX_LEN + 1]; Ops O[MAX_LEN + 1][MAX_LEN + 1];
    for (int i = 0; i <= n; i++) for (int j = 0; j <= m; j++) { D[i][j] = BIG; O[i][j] = Ops{0, 0, 0, 0}; }
    D[0][0] = 0;
    for (int i = 1; i <= n; i++) { D[i][0] = i; O[i][0] = Ops{0, (int8_t)i, 0, 0}; }
    for (int j = 1; j <= m; j++) { D[0][j] = j; O[0][j] = Ops{0, 0, (int8_t)j, 0}; }
    for (int i = 1; i <= n; i++) {
        // band: a path of cost <= max_edits never leaves |i - j| <= max_edits (every off-diagonal step costs 1,
        // a transposition moves diagonally); cells outside stay BIG and are never on a surviving path
        int jlo = std::max(1, i - max_edits), jhi = std::min(m, i + max_edits);
        for (int j = jlo; j <= jhi; j++) {
            int best = BIG; Ops bo{0, 0, 0, 0};
            int a = obs[i - 1], b = cand[j - 1];
            int c; Ops o;
            if (a == b) { c = D[i - 1][j - 1]; o = O[i - 1][j - 1]; }
            else if (a && b && nb.is_neighbor(a, b)) { c = D[i - 1][j - 1] + 1; o = O[i - 1][j - 1]; o.sub++; }
            else { c = BIG; }
            if (c < best) { best = c; bo = o; }
            if (i > 1 && j > 1 && obs[i - 1] == cand[j - 2] && obs[i - 2] == cand[j - 1] && obs[i - 1] != obs[i - 2]) {
                c = D[i - 2][j - 2] + 1;
                if (c < best) { best = c; bo = O[i - 2][j - 2]; bo.tr++; }
            }
            c = D[i][j - 1] + 1;
            if (c < best) { best = c; bo = O[i][j - 1]; bo.del++; }
            c = D[i - 1][j] + 1;
            if (c < best) { best = c; bo = O[i - 1][j]; bo.ins++; }
            D[i][j] = std::min(best, BIG); O[i][j] = bo;
        }
        // early exit: costs never decrease along a path and every later cell depends on rows i-1 and i
        // only (transposition reaches back two rows), so if both rows exceed the cutoff the result is BIG
        if (i >= 2) {
            int rmin = BIG;
            for (int j = 0; j <= m; j++) { if (D[i][j] < rmin) rmin = D[i][j]; if (D[i - 1][j] < rmin) rmin = D[i - 1][j]; }
            if (rmin > max_edits) return EditResult{BIG, Ops{0, 0, 0, 0}};
        }
    }
    int d = D[n][m];
    if (d > max_edits) return EditResult{BIG, Ops{0, 0, 0, 0}};
    return EditResult{d, O[n][m]};
}

void edit_all(const int* obs, int n, const SyllableTable& tab, const NeighborMatrix& nb, int max_edits, int* edits_out, int8_t* ops_out) {
    for (int s = 0; s < tab.count; s++) {
        int m = tab.len[s];
        if (m - n > max_edits || n - m > max_edits) { edits_out[s] = BIG; std::memset(ops_out + 4 * s, 0, 4); continue; }
        EditResult r = edit_one(obs, n, tab.ids + s * tab.max_len, m, nb, max_edits);
        edits_out[s] = r.edits; ops_out[4 * s] = r.ops.sub; ops_out[4 * s + 1] = r.ops.ins; ops_out[4 * s + 2] = r.ops.del; ops_out[4 * s + 3] = r.ops.tr;
    }
}

}  // namespace aq

namespace aq {

double channel_one(const int* obs, int n, const int* cand, int m, const NeighborMatrix& nb, const ChannelCosts& cc, int max_edits) {
    // C[i][j][e] = cheapest alignment of obs[:i] with cand[:j] using exactly e edits (e <= max_edits). Move set and the
    // neighbour gate are those of edit_one; the answer is min over e, i.e. the channel -log likelihood of the typed keys
    // given the syllable, restricted to the menu's edit budget. Loop order and summation order mirror ref_edit.channel_one
    // so both produce bit-identical doubles.
    if (max_edits > MAX_LEN) max_edits = MAX_LEN;
    static thread_local double C[MAX_LEN + 1][MAX_LEN + 1][MAX_LEN + 1];
    for (int i = 0; i <= n; i++) for (int j = 0; j <= m; j++) for (int e = 0; e <= max_edits; e++) C[i][j][e] = BIGC;
    C[0][0][0] = 0.0;
    for (int i = 1; i <= n && i <= max_edits; i++) C[i][0][i] = C[i - 1][0][i - 1] + cc.ins_cost(obs[i - 1], cand[0]);
    for (int j = 1; j <= m && j <= max_edits; j++) C[0][j][j] = C[0][j - 1][j - 1] + cc.del;
    for (int i = 1; i <= n; i++) {
        for (int j = 1; j <= m; j++) {
            int a = obs[i - 1], b = cand[j - 1];
            bool sub_ok = (a != b) && a && b && nb.is_neighbor(a, b); double sc = sub_ok ? cc.sub_cost(a, b) : BIGC;
            bool tr_ok = i > 1 && j > 1 && obs[i - 1] == cand[j - 2] && obs[i - 2] == cand[j - 1] && obs[i - 1] != obs[i - 2];
            double ic = cc.ins_cost(a, j < m ? cand[j] : cand[m - 1]);      // extra key a typed before cand[j] (trailing: the last intended key)
            for (int e = 0; e <= max_edits; e++) {
                double best = BIGC;
                if (a == b) best = C[i - 1][j - 1][e];
                if (e >= 1) {
                    if (sub_ok) { double c = C[i - 1][j - 1][e - 1] + sc; if (c < best) best = c; }
                    if (tr_ok) { double c = C[i - 2][j - 2][e - 1] + cc.tr; if (c < best) best = c; }
                    { double c = C[i][j - 1][e - 1] + cc.del; if (c < best) best = c; }
                    { double c = C[i - 1][j][e - 1] + ic; if (c < best) best = c; }
                }
                C[i][j][e] = best;
            }
        }
    }
    double r = BIGC; for (int e = 0; e <= max_edits; e++) if (C[n][m][e] < r) r = C[n][m][e];
    return r;
}

void channel_all(const int* obs, int n, const SyllableTable& tab, const NeighborMatrix& nb, const ChannelCosts& cc, int max_edits, double* cost_out) {
    for (int s = 0; s < tab.count; s++) {
        int m = tab.len[s];
        if (m - n > max_edits || n - m > max_edits) { cost_out[s] = BIGC; continue; }
        cost_out[s] = channel_one(obs, n, tab.ids + s * tab.max_len, m, nb, cc, max_edits);
    }
}

int16_t channel_fixed(double cost, double scale) {
    double v = std::floor(cost * scale + 0.5); if (v > 32767.0) v = 32767.0; if (v < 0.0) v = 0.0; return (int16_t)v;
}

}  // namespace aq
