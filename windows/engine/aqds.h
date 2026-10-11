// The fallback dictionary store (AQDS v2), as Sources/AquaTypeUIContract/AQDSStore.swift reads it: the file is checked
// against its pinned SHA-256, its header and every section's bounds at open; a lookup is an exact match on the raw key
// string through the FNV-1a hash table.
#pragma once
#include <memory>
#include <string>
#include <vector>
#include "util.h"

namespace aqw {

struct AQDSCandidate { std::string text; double weight; uint8_t source_id; std::string source_name; };

class AQDSFallbackStore {
public:
    static constexpr const char* accepted_sha256 = "6fbe62f2359ad4a1c239ac7124adb5c1d135bf584239022c91b7e15342b586ea";
    AQDSFallbackStore(const std::wstring& binary, const std::wstring& manifest);   // throws on any mismatch
    std::vector<AQDSCandidate> exact_rows(const std::string& raw, int limit) const;
private:
    uint64_t u64(size_t off) const; uint32_t u32(size_t off) const; uint32_t u24(size_t off) const; uint16_t u16(size_t off) const; uint8_t u8(size_t off) const;
    void require(size_t off, size_t len) const;
    struct Raw { size_t offset, length, row_start, rows; };
    Raw raw_record(size_t index) const;
    bool raw_index(const std::string& raw, size_t* index) const;
    std::unique_ptr<MappedFile> m_;
    std::vector<std::string> source_names_;
    size_t raw_hash_off_ = 0, raw_hash_slots_ = 0, raw_index_off_ = 0, raw_count_ = 0, rows_off_ = 0, row_count_ = 0, raw_blob_off_ = 0, raw_blob_bytes_ = 0, text_blob_off_ = 0, text_blob_bytes_ = 0;
};

}  // namespace aqw
