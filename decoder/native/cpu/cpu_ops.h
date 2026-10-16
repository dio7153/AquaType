// The CPU backend's building blocks: a safetensors reader, a small host array that stands in for the few MLX calls the
// session makes (AQ_CPU builds), fp16 conversion, and the kernels the two models need - fp32 GEMM, GEMM against MLX's
// 4-bit affine-quantised weights, layer norm, GELU, exp - on AVX2/FMA, or SSE4.1 in the build for older CPUs
// (aq_dispatch.cpp), with OpenMP threads.
#pragma once
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace aq {
namespace cpu {

enum class Dtype { f32, f16, u32, i32, u8, i8 };
size_t dtype_size(Dtype t);

// A host array: shape, element type and shared storage.  Only what the session reads from it: shape(i), data<T>(), size().
class array {
public:
    array() = default;
    array(Dtype t, std::vector<int> shape);
    array(Dtype t, std::vector<int> shape, std::shared_ptr<std::vector<uint8_t>> buf, size_t offset);
    int shape(int i) const { return shape_.at((size_t)i); }
    const std::vector<int>& shape() const { return shape_; }
    int ndim() const { return (int)shape_.size(); }
    size_t size() const { size_t n = 1; for (int s : shape_) n *= (size_t)s; return n; }
    Dtype dtype() const { return dtype_; }
    template <class T> T* data() { return reinterpret_cast<T*>(buf_->data() + off_); }
    template <class T> const T* data() const { return reinterpret_cast<const T*>(buf_->data() + off_); }
private:
    Dtype dtype_ = Dtype::f32; std::vector<int> shape_; std::shared_ptr<std::vector<uint8_t>> buf_; size_t off_ = 0;
};

// The MLX calls v2_session.cpp makes, as no-ops or host equivalents.
inline void eval(const array&) {}
template <class... A> inline void eval(const array&, const A&...) {}
inline void async_eval(const array&) {}
inline void clear_cache() {}
inline void set_cache_limit(size_t) {}
// name -> tensor; the second member mirrors MLX's metadata map and is always empty
std::pair<std::map<std::string, array>, std::map<std::string, std::string>> load_safetensors(const std::string& path);

// ---- fp16
float h2f(uint16_t h);
uint16_t f2h(float f);
void h2f_n(const uint16_t* src, float* dst, size_t n);
void f2h_n(const float* src, uint16_t* dst, size_t n);
std::vector<float> to_f32(const array& a);       // f16 / f32 tensor -> fp32 vector

// ---- MLX affine quantisation: each uint32 holds 32/bits values, lowest bits first; w = q * scale + bias per group.
struct QWeight {
    const uint32_t* q = nullptr; std::vector<float> scales, biases;   // scales/biases [out, in / group]
    int out = 0, in = 0, bits = 4, group = 64;
    array keep;                                                       // owns the packed storage
    void dequant_row(int r, float* dst) const;                        // one output row, fp32 [in]
};

// ---- kernels (row-major; "nt" = the weight is [m, K], the dot product runs along K for both operands)
// y[n, m] = x[n, K] . w[m, K]^T (+ bias[m]) (+ y when accumulate)
void gemm_nt(const float* x, int n, int K, const float* w, int m, float* y, const float* bias = nullptr, bool accumulate = false);
// a weight packed once for gemm_packed: panels of 16 output columns, k-major inside a panel
struct Packed { int m = 0, K = 0; std::vector<float> p; };
Packed pack_nt(const float* w, int m, int K);
void gemm_packed(const float* x, int n, const Packed& w, float* y, const float* bias = nullptr, bool accumulate = false);
// the same against a quantised weight; rows (optional) picks the weight rows to use, giving y[n, rows.size()]
void gemm_q(const float* x, int n, const QWeight& w, float* y, const float* bias = nullptr, const std::vector<int32_t>* rows = nullptr);
// an int8 copy of an fp32 weight [m, K]: symmetric per group of 64 along K (w ~ q * scale), K a multiple of 64.
// gemm_i8 quantises x the same way and multiplies on integers (maddubs); y as gemm_packed.
struct I8Weight { int out = 0, in = 0; std::vector<int8_t> q; std::vector<float> scales; };
I8Weight quant_weight_i8(const float* w, int m, int K);
void gemm_i8(const float* x, int n, const I8Weight& w, float* y, const float* bias = nullptr, bool accumulate = false);
void layer_norm(const float* x, int n, int d, const float* g, const float* b, float eps, float* y);
void gelu_inplace(float* x, size_t n);           // 0.5 x (1 + erf(x / sqrt 2)), MLX's erf polynomial
float logsumexp_row(const float* x, int n);
// int8 rows of the K/V table: quantise by absmax / 127 (returns the scale), dot with an fp32 query, acc = acc * c + w * v
float quant_i8(const float* src, int n, int8_t* dst);
float dot_f32_i8(const float* q, const int8_t* k, int n);
void scale_add_i8(float* acc, float c, float w, const int8_t* v, int n);
void log_softmax_rows(float* x, int n, int d);

int num_threads();
}  // namespace cpu
}  // namespace aq
