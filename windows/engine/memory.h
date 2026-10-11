// The personal memory, as Sources/AquaTypeEngine/PersonalMemory.swift keeps it (see that file for the reasoning behind
// every rule): exact recall under the typed and the clean keys, fusion by reading, corrections and demotion from the
// commit stream, one file (personal_memory.v2.tsv) re-read under an exclusive lock before every write and replaced
// atomically.  The Windows parts: LockFileEx on a lock file beside it, MoveFileEx over the old file, the file's index
// and write time as its identity.
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace aqw {

struct MemoryRecord {
    std::string text, typed_keys, clean_keys;
    std::vector<int32_t> readings;
    double count = 1; int64_t last_seen = 0;
};

class PersonalMemoryStore {
public:
    static constexpr double confirmed_count = 2;
    static constexpr int min_fusion_readings = 2, display_per_key = 3, max_rows = 4000;
    static constexpr int64_t correction_window = 60;

    explicit PersonalMemoryStore(const std::string& state_dir);
    const std::vector<MemoryRecord>& rows() const { return rows_; }
    double support(const MemoryRecord& r, int64_t now) const;
    static bool consistent(const std::string& text, const std::vector<int32_t>& readings);
    void record(const std::string& text, const std::string& typed, const std::string& clean, const std::vector<int32_t>& readings, int64_t now);
    double count(const std::string& typed, const std::string& text) const;
    void retract(const std::string& typed, const std::string& text);
    void demote(const std::string& keys, const std::string& chosen);
    void note_outcome(const std::string& client, const std::string& typed, const std::string& text, int64_t now);
    std::vector<std::string> exact_recall(const std::string& keys, int64_t now) const;
    bool fuse(const std::string& text, const std::vector<int32_t>& readings, int64_t now, std::string* out) const;
    void transaction(const std::function<void()>& changes);
private:
    struct Identity { uint32_t volume = 0; uint64_t index = 0, size = 0, mtime = 0; bool operator==(const Identity& o) const { return volume == o.volume && index == o.index && size == o.size && mtime == o.mtime; } };
    bool identity_on_disk(Identity* id) const;
    void remove_stale_temporaries();
    void read_rows_from_disk();
    void refresh_from_disk();
    void save();
    void with_exclusive_lock(const std::function<void()>& body);
    void mutate(const std::function<void()>& change);
    void reindex();
    void prune(int64_t now);

    std::wstring path_, folder_, name_;
    std::vector<MemoryRecord> rows_;
    std::unordered_map<std::string, std::vector<size_t>> by_typed_, by_clean_;
    std::unordered_map<int32_t, std::vector<size_t>> by_first_reading_;
    bool unreadable_ = false, not_looked_at_ = false, in_transaction_ = false, lock_unavailable_reported_ = false;
    bool have_seen_ = false; Identity last_seen_on_disk_;
    struct Last { std::string keys, text; int64_t at; };
    std::map<std::string, Last> last_commit_;
};

}  // namespace aqw
