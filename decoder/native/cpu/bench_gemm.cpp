// bench_gemm: the CPU backend's GEMM kernels alone, at the shapes the two models use.
#include "cpu_ops.h"
#include <chrono>
#include <cstdio>
#include <random>
using namespace aq::cpu;
template <class F> static double ms(F f, int reps = 20) { f(); auto t = std::chrono::steady_clock::now(); for (int i = 0; i < reps; i++) f(); return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count() / reps; }
int main() {
    std::mt19937 g(1); std::uniform_real_distribution<float> u(-1, 1);
    auto rnd = [&](size_t n) { std::vector<float> v(n); for (auto& x : v) x = u(g); return v; };
    struct S { int n, K, m; } shapes[] = {{1000, 384, 1152}, {1000, 1536, 384}, {256, 384, 1536}, {1000, 384, 2048}, {40, 1024, 4096}};
    std::printf("threads %d\n", num_threads());
    for (auto s : shapes) {
        auto x = rnd((size_t)s.n * s.K), w = rnd((size_t)s.m * s.K); std::vector<float> y((size_t)s.n * s.m), b(s.m, 0.f);
        Packed P = pack_nt(w.data(), s.m, s.K); const double gf = 2.0 * s.n * s.K * s.m / 1e9;
        double t1 = ms([&] { gemm_nt(x.data(), s.n, s.K, w.data(), s.m, y.data(), b.data()); });
        double t2 = ms([&] { gemm_packed(x.data(), s.n, P, y.data(), b.data()); });
        std::printf("n %4d K %4d m %4d: nt %6.2f ms (%5.0f GF/s)  packed %6.2f ms (%5.0f GF/s)\n", s.n, s.K, s.m, t1, gf / t1 * 1e3, t2, gf / t2 * 1e3);
    }
}