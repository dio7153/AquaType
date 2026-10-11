#include "aqds.h"
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <nlohmann/json.hpp>

namespace aqw {
using json = nlohmann::json;
static std::runtime_error bad(const std::string& s) { return std::runtime_error(s); }

void AQDSFallbackStore::require(size_t off, size_t len) const { if (off > m_->size() || len > m_->size() - off) throw bad("AQDS range out of bounds"); }
uint8_t AQDSFallbackStore::u8(size_t o) const { require(o, 1); return m_->data()[o]; }
uint16_t AQDSFallbackStore::u16(size_t o) const { require(o, 2); const uint8_t* p = m_->data() + o; return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t AQDSFallbackStore::u24(size_t o) const { require(o, 3); const uint8_t* p = m_->data() + o; return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16); }
uint32_t AQDSFallbackStore::u32(size_t o) const { require(o, 4); uint32_t v; std::memcpy(&v, m_->data() + o, 4); return v; }
uint64_t AQDSFallbackStore::u64(size_t o) const { require(o, 8); uint64_t v; std::memcpy(&v, m_->data() + o, 8); return v; }

static std::string hex(const uint8_t* p, size_t n) { static const char* X = "0123456789abcdef"; std::string o; for (size_t i = 0; i < n; i++) { o += X[p[i] >> 4]; o += X[p[i] & 15]; } return o; }

AQDSFallbackStore::AQDSFallbackStore(const std::wstring& binary, const std::wstring& manifest_path) {
    const json m = json::parse(read_file(manifest_path));
    if (m.at("format") != "aquatype-dict-compact" || m.at("version") != 2 || m.at("endian") != "little" || m.at("kind") != "fallback"
        || m.at("binary_sha256") != accepted_sha256 || m.at("builder").at("implementation_sha256") != "4c611268ba579e8f0d7eff687c8fb5b904e29461a2ad9cfe4947ed24eda20add")
        throw bad("AQDS manifest identity mismatch");
    if (sha256_file_hex(binary) != accepted_sha256) throw bad("AQDS binary SHA mismatch");
    m_ = std::make_unique<MappedFile>(binary);
    if (m_->size() != m.at("binary_bytes").get<size_t>() || std::memcmp(m_->data(), "AQDICT1\0", 8) != 0 || u32(8) != 2 || u32(12) != 0x01020304 || u32(20) != 256 || u64(24) != m_->size())
        throw bad("AQDS binary header mismatch");
    if (hex(m_->data() + 32, 32) != m.at("payload_sha256") || hex(m_->data() + 64, 32) != m.at("source_aggregate_sha256")) throw bad("AQDS header/manifest digest mismatch");
    if (u32(16) != 1) throw bad("fallback AQDS kind");
    for (auto& s : m.at("sources")) source_names_.push_back(s.at("name").get<std::string>());
    raw_index_off_ = (size_t)u64(96); raw_count_ = (size_t)u64(104); rows_off_ = (size_t)u64(112); row_count_ = (size_t)u64(120);
    raw_blob_off_ = (size_t)u64(144); raw_blob_bytes_ = (size_t)u64(152); text_blob_off_ = (size_t)u64(160); text_blob_bytes_ = (size_t)u64(168);
    raw_hash_off_ = (size_t)u64(216); raw_hash_slots_ = (size_t)u64(224);
    const json& sec = m.at("sections");
    auto at = [&](const char* k) { return sec.at(k).at("offset").get<size_t>(); };
    if (raw_hash_slots_ == 0 || (raw_hash_slots_ & (raw_hash_slots_ - 1)) != 0 || raw_hash_off_ != 256 || at("raw_hash") != raw_hash_off_ || at("raw_index") != raw_index_off_
        || at("rows") != rows_off_ || at("raw_blob") != raw_blob_off_ || at("text_blob") != text_blob_off_
        || raw_blob_off_ > m_->size() || raw_blob_bytes_ > m_->size() - raw_blob_off_ || text_blob_off_ + text_blob_bytes_ != m_->size())
        throw bad("fallback AQDS section layout");
}

AQDSFallbackStore::Raw AQDSFallbackStore::raw_record(size_t index) const {
    if (index >= raw_count_) throw bad("fallback raw index out of range");
    const size_t b = raw_index_off_ + index * 16;
    Raw r{u32(b), u16(b + 4), u32(b + 8), u32(b + 12)};
    if (u16(b + 6) != 0 || r.offset > raw_blob_bytes_ || r.length > raw_blob_bytes_ - r.offset || r.row_start > row_count_ || r.rows > row_count_ - r.row_start) throw bad("corrupt fallback raw record");
    return r;
}

bool AQDSFallbackStore::raw_index(const std::string& raw, size_t* index) const {
    uint64_t h = 0xcbf29ce484222325ull; for (unsigned char c : raw) { h ^= c; h *= 0x100000001b3ull; }
    const uint32_t fp = (uint32_t)((h >> 60) & 0xF); size_t slot = (size_t)(h & (raw_hash_slots_ - 1));
    for (size_t n = 0; n < raw_hash_slots_; n++) {
        const uint32_t enc = u24(raw_hash_off_ + slot * 3);
        if (enc == 0) return false;
        const size_t ip1 = enc & 0xFFFFF; if (ip1 == 0 || ip1 > raw_count_) throw bad("corrupt fallback hash index");
        if ((enc >> 20) == fp) { const Raw r = raw_record(ip1 - 1);
            if (r.length == raw.size() && std::memcmp(m_->data() + raw_blob_off_ + r.offset, raw.data(), raw.size()) == 0) { *index = ip1 - 1; return true; } }
        slot = (slot + 1) & (raw_hash_slots_ - 1);
    }
    throw bad("fallback raw hash probe exhausted");
}

std::vector<AQDSCandidate> AQDSFallbackStore::exact_rows(const std::string& raw, int limit) const {
    std::vector<AQDSCandidate> out; size_t idx;
    if (limit <= 0 || !raw_index(raw, &idx)) return out;
    const Raw r = raw_record(idx);
    for (size_t i = r.row_start; i < r.row_start + std::min((size_t)limit, r.rows); i++) {
        const size_t b = rows_off_ + i * 16; const size_t off = u32(b), len = u16(b + 4); const uint8_t src = u8(b + 6), reserved = u8(b + 7);
        double w; uint64_t bits = u64(b + 8); std::memcpy(&w, &bits, 8);
        if (reserved != 0 || src >= source_names_.size() || off > text_blob_bytes_ || len > text_blob_bytes_ - off || !std::isfinite(w)) throw bad("corrupt fallback row");
        std::string t((const char*)m_->data() + text_blob_off_ + off, len);
        if (!valid_utf8(t)) throw bad("AQDS row text is not UTF-8");
        out.push_back({t, w, src, source_names_[src]});
    }
    return out;
}

}  // namespace aqw
